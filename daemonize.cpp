/**
 * Copyright [2016]
 *
 * \author [Artur Troian <troian dot ap at gmail dot com>]
 * \author [Oleg Kravchenko <troian dot ap at gmail dot com>]
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string>
#include <iostream>
#include <fstream>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <boost/filesystem.hpp>

#include <daemon/daemonize.h>

static Json::Value *g_config = nullptr;
static int *g_lock_fd        = nullptr;
static cleanup_cb cleanup    = nullptr;
static void *cleanup_ctx     = nullptr;

void exit_daemon(int err)
{
	if (cleanup)
		cleanup(cleanup_ctx);

	if (g_lock_fd) {
		if (*g_lock_fd > 0) {
			if (flock(*g_lock_fd, LOCK_UN) != 0) {
				std::cerr << "Can't unlock the lock file";
			}
			close(*g_lock_fd);
			*g_lock_fd = 0;
		}

		delete g_lock_fd;
	}

	unlink(g_config->operator[]("pid_file").asString().c_str());

	delete g_config;

	_exit(err);
}

static void already_running(const std::string &lock_file)
{
	*g_lock_fd = open(lock_file.c_str(), O_RDONLY, S_IRUSR | S_IWUSR);
	if (*g_lock_fd <= 0) {
		fprintf(stderr, "Can't open executable to lock: \"%s\": %s\n", lock_file.c_str(), strerror(errno));
		exit_daemon(EXIT_FAILURE);
	}

	if (flock(*g_lock_fd, LOCK_EX | LOCK_NB) != 0) {
		fprintf(stderr, "Can't lock the lock file \"%s\". Is another instance running?\n", lock_file.c_str());
		exit_daemon(EXIT_FAILURE);
	}
}

static void write_pid(const std::string &pid_file)
{
	try {
		std::ofstream file(pid_file, std::ios_base::trunc);
		file << std::to_string(getpid());
	} catch (const std::exception &e) {
		exit_daemon(EXIT_FAILURE);
	}
}

static void verify_config(Json::Value *config)
{
	if (config->isMember("lock_file") == false) {
		std::cerr << "Daemon config must provide \"lock_file\" member";
		exit_daemon(EXIT_FAILURE);
	}

	if (config->isMember("env_dir") == false) {
		std::cerr << "Daemon config must provide \"env_dir\" member";
		exit_daemon(EXIT_FAILURE);
	}

	if (config->isMember("pid_file") == false) {
		std::cerr << "Daemon config must provide \"pid_file\" member";
		exit_daemon(EXIT_FAILURE);
	}

	if (config->isMember("as_daemon") == false) {
		std::cerr << "Daemon config must provide \"as_daemon\" member";
		exit_daemon(EXIT_FAILURE);
	}
}

pid_t make_daemon(Json::Value *config, cleanup_cb cb, void *userdata)
{
	g_lock_fd   = new int;
	g_config    = config;
	cleanup     = cb;
	cleanup_ctx = userdata;

	int fds[2];

	verify_config(config);

	already_running(config->operator[]("lock_file").asString());

	if (config->operator[]("as_daemon").asBool() == true) {
		pid_t            pid;
		struct sigaction sa;

//		umask(0);

		/* create pipe to retrive pid of daemon */
		if (pipe(fds)) {
			exit_daemon(EXIT_FAILURE);
		}


		if ((pid = fork())) {
			/* close pipe for writting now to avoid deadlock, if child failed */
			close(fds[1]);

			if (pid == -1) {
				/* can't fork, return error */
				goto fork1_done;
			}

			int   status;
			pid_t rc;

			/* wait until child process finish with daemon startup */
			while (-1 == (rc = waitpid(pid, &status, 0))) {
				/* ignore POSIX signals */
				if (EINTR != errno) {
					pid = -1;

					goto fork1_done;
				}
			}

			/* daemon startup failed */
			if (EXIT_FAILURE == WEXITSTATUS(status)) {
				pid = -1;

				goto fork1_done;
			}

			/* read pid of daemon from pipe */
			if (sizeof(pid) != read(fds[0], &pid, sizeof(pid))) {
				pid = -1;
			}

		fork1_done:
			close(fds[0]);

			return pid;
		}

		sa.sa_handler = SIG_IGN;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = 0;
		if (sigaction(SIGHUP, &sa, NULL) < 0) {
			std::cerr << "Unable to ignore signal SIGHUP. Error: " << strerror(errno) << std::endl;
			exit_daemon(EXIT_FAILURE);
		}

		/* second fork for daemon startup */
		if ((pid = fork())) {
			if (pid == -1) {
				/* fail to fork, report about it */
				_exit(EXIT_FAILURE);
			}

			/* report about successful attempt running of daemon */
			_exit(EXIT_SUCCESS);
		}

		/* detach to init process */
		if (setsid() < 0) {
			exit_daemon(EXIT_FAILURE);
		}

		/* report our pid... */
		pid = getpid();

		/* to avoid duplicates of daemon */
		if (sizeof(pid) != write(fds[1], &pid, sizeof(pid))) {
			exit_daemon(EXIT_FAILURE);
		}
	}

	// Close all of filedescriptors
	/* retrieve maximum fd number */
	int max_fds = getdtablesize();

	if (max_fds == -1) {
		exit_daemon(EXIT_FAILURE);
	}

	/* close all fds, except standard (in, out and err) streams */
	for (int fd = 3; fd < max_fds; ++fd) {
		struct stat st;
		std::memset(&st, 0, sizeof(struct stat));

		if (fstat(fd, &st)) {
			/* fd not used */
			continue;
		}

		if (close(fd)) {
			exit_daemon(EXIT_FAILURE);
		}
	}

	// Setup environment dir
	if (chdir(config->operator[]("env_dir").asString().c_str()) < 0) {
		exit_daemon(EXIT_FAILURE);
	}

	// check of log directory exists
	std::string log_path(config->operator[]("env_dir").asString());
	log_path += config->operator[]("log")["dir"].asString();
	if (!boost::filesystem::exists(log_path)) {
		boost::filesystem::create_directory(log_path);
	}

	std::string std_file;
	Json::Value io_config;

	if (config->operator[]("io_mode").asString().compare("io_daemon") == 0) {
		io_config = config->operator[]("io_daemon");
	} else {
		io_config = config->operator[]("io_debug");
	}

	if (io_config["stdin"].asString().compare("stdin") != 0) {
		// stdin needs redirection
		if (io_config["stdin"].asString().compare("/dev/null") == 0) {
			std_file = "/dev/null";
		} else {
			std_file = config->operator[]("log")["dir"].asString();
			std_file.append("/");
			std_file.append(io_config["stdin"].asString());
		}

		fflush(stdin);
		close(STDIN_FILENO);

		int stdin_fd = open(std_file.c_str(), O_RDONLY);

		if (stdin_fd != 0) {
			if (stdin_fd > 0)
				close(stdin_fd);
			fprintf(stderr, "Unable to redirect stdin: Opened to: %d. Error: %s", stdin_fd, strerror(errno));
			exit_daemon(EXIT_FAILURE);
		}
	}

	if (io_config["stdout"].asString().compare("stdout") != 0) {
		// stdout needs redirection
		if (io_config["stdout"].asString().compare("/dev/null") == 0) {
			std_file = "/dev/null";
		} else {
			std_file = config->operator[]("log")["dir"].asString();
			std_file.append("/");
			std_file.append(io_config["stdout"].asString());
		}

		fflush(stdout);
		close(STDOUT_FILENO);

		int stdout_fd = open(std_file.c_str(), O_CREAT | O_WRONLY | O_TRUNC);
		if (stdout_fd != 1) {
			if (stdout_fd > 0)
				close(stdout_fd);
			fprintf(stderr, "Unable to redirect stdout: Opened to: %d. Error: %s\n", stdout_fd, strerror(errno));
			exit_daemon(EXIT_FAILURE);
		}

		if (std_file.compare("/dev/null") != 0) {
			if (chmod(std_file.c_str(), 0644) < 0) {
				fprintf(stderr, "Unable change file permision: [%s]. Reason: %s\n", std_file.c_str(), strerror(errno));
				exit_daemon(EXIT_FAILURE);
			}
		}
	}

	if (io_config["stderr"].asString().compare("stdout") != 0) {
		// stderr needs redirection
		if (io_config["stderr"].asString().compare("/dev/null") == 0) {
			std_file = "/dev/null";
		} else {
			std_file = config->operator[]("log")["dir"].asString();
			std_file.append("/");
			std_file.append(io_config["stderr"].asString());
		}

		fflush(stderr);
		close(STDERR_FILENO);

		int stderr_fd = open(std_file.c_str(), O_CREAT | O_WRONLY | O_TRUNC);

		if (stderr_fd != 2) {
			if (stderr_fd > 0)
				close(stderr_fd);
			fprintf(stderr, "Unable to redirect stderr: Opened to: %d. Error: %s", stderr_fd, strerror(errno));
			exit_daemon(EXIT_FAILURE);
		}

		if (std_file.compare("/dev/null") != 0) {
			if (chmod(std_file.c_str(), 0644) < 0) {
				fprintf(stderr, "Unable change file permision: [%s]. Reason: %s", std_file.c_str(), strerror(errno));
				exit_daemon(EXIT_FAILURE);
			}
		}
	}

	struct rlimit core_limits;
	core_limits.rlim_cur = core_limits.rlim_max = RLIM_INFINITY;

	if (setrlimit(RLIMIT_CORE, &core_limits) < 0) {
		fprintf(stderr, "Unable to set rlimits. Error: %s", strerror(errno));
		exit_daemon(EXIT_FAILURE);
	}

	write_pid(config->operator[]("pid_file").asString());

	return 0;
}
