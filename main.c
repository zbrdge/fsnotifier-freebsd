/*
 * Copyright 2000-2010 JetBrains s.r.o.
 * * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fsnotifier.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/ucred.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/select.h>
#include <syslog.h>
#include <unistd.h>

#define LOG_ENV "FSNOTIFIER_LOG_LEVEL"
#define LOG_ENV_DEBUG "debug"
#define LOG_ENV_INFO "info"
#define LOG_ENV_WARNING "warning"
#define LOG_ENV_ERROR "error"
#define LOG_ENV_OFF "off"

#define USAGE_MSG \
    "fsnotifier - IntelliJ IDEA companion program for watching and reporting file and directory structure modifications.\n\n" \
    "fsnotifier utilizes \"user\" facility of syslog(3) - messages usually can be found in /var/log/user.log.\n" \
    "Verbosity is regulated via " LOG_ENV " environment variable, possible values are: " \
    LOG_ENV_DEBUG ", " LOG_ENV_INFO ", " LOG_ENV_WARNING ", " LOG_ENV_ERROR ", " LOG_ENV_OFF "; latter is the default.\n\n" \
    "Use 'fsnotifier --selftest' to perform some self-diagnostics (output will be logged and printed to console).\n"

#define HELP_MSG \
    "Try 'fsnotifier --help' for more information.\n"

array* ROOTS = NULL;
array* UNWATCHABLE = NULL;

static bool show_warning = true;

static bool self_test = false;

int level = LOG_EMERG;

#define CHECK_NULL(p) if (p == NULL)  { userlog(LOG_ERR, "out of memory"); return false; }

static void init_log();
static void run_self_test();
static void main_loop();
static bool read_input();
static bool update_roots(array* new_roots);
static void unregister_roots();
static bool register_roots(array* new_roots, array* unwatchable);
static bool unwatchable_mounts(array* mounts);
static void inotify_callback(char* path, int event);


int main(int argc, char** argv) {
  if (argc > 1) {
    if (strcmp(argv[1], "--help") == 0) {
      printf(USAGE_MSG);
      return 0;
    }
    else if (strcmp(argv[1], "--selftest") == 0) {
      self_test = true;
    }
    else {
      printf("unrecognized option: %s\n", argv[1]);
      printf(HELP_MSG);
      return 1;
    }
  }

  init_log();
  if (!self_test) {
    userlog(LOG_INFO, "started");
  }
  else {
    userlog(LOG_INFO, "started (self-test mode)");
  }

  setvbuf(stdin, NULL, _IONBF, 0);
  setvbuf(stdout, NULL, _IONBF, 0);

  ROOTS = array_create(20);
  if (init_inotify() && ROOTS != NULL) {
    set_inotify_callback(&inotify_callback);

    if (!self_test) {
      main_loop();
    }
    else {
      run_self_test();
    }

    unregister_roots();
  }
  else {
    printf("GIVEUP\n");
  }
  close_inotify();
  array_delete(ROOTS);

  userlog(LOG_INFO, "finished");
  closelog();

  return 0;
}


static void init_log() {
  char* env_level = getenv(LOG_ENV);
  if (env_level != NULL) {
    if (strcmp(env_level, LOG_ENV_DEBUG) == 0)  level = LOG_DEBUG;
    else if (strcmp(env_level, LOG_ENV_INFO) == 0)  level = LOG_INFO;
    else if (strcmp(env_level, LOG_ENV_WARNING) == 0)  level = LOG_WARNING;
    else if (strcmp(env_level, LOG_ENV_ERROR) == 0)  level = LOG_ERR;
  }

  if (self_test) {
    level = LOG_DEBUG;
  }

  char ident[32];
  snprintf(ident, sizeof(ident), "fsnotifier[%d]", getpid());
  openlog(ident, 0, LOG_USER);
  setlogmask(LOG_UPTO(level));
}


void userlog(int priority, const char* format, ...) {
  va_list ap;

  va_start(ap, format);
  vsyslog(priority, format, ap);
  va_end(ap);

  if (self_test) {
    const char* level = "debug";
    switch (priority) {
      case LOG_ERR:  level = "error"; break;
      case LOG_WARNING:  level = " warn"; break;
      case LOG_INFO:  level = " info"; break;
    }
    printf("fsnotifier[%d] %s: ", getpid(), level);

    va_start(ap, format);
    vprintf(format, ap);
    va_end(ap);

    printf("\n");
  }
}


static void run_self_test() {
  array* test_roots = array_create(1);
  char* cwd = malloc(PATH_MAX);
  if (getcwd(cwd, PATH_MAX) == NULL) {
    strncpy(cwd, ".", PATH_MAX);
  }
  array_push(test_roots, cwd);
  update_roots(test_roots);
}


static void main_loop() {
  int input_fd = fileno(stdin), inotify_fd = get_inotify_fd();
  int nfds = (inotify_fd > input_fd ? inotify_fd : input_fd) + 1;
  fd_set rfds;
  bool go_on = true;

  while (go_on) {
    FD_ZERO(&rfds);
    FD_SET(input_fd, &rfds);
    FD_SET(inotify_fd, &rfds);
    if (select(nfds, &rfds, NULL, NULL, NULL) < 0) {
      userlog(LOG_ERR, "select: %s", strerror(errno));
      go_on = false;
    }
    else if (FD_ISSET(input_fd, &rfds)) {
      go_on = read_input();
    }
    else if (FD_ISSET(inotify_fd, &rfds)) {
      go_on = process_inotify_input();
    }
  }
}


static bool read_input() {
  char* line = read_line(stdin);
  userlog(LOG_DEBUG, "input: %s", (line ? line : "<null>"));

  if (line == NULL || strcmp(line, "EXIT") == 0) {
    return false;
  }

  if (strcmp(line, "ROOTS") == 0) {
    array* new_roots = array_create(20);
    CHECK_NULL(new_roots);

    while (1) {
      line = read_line(stdin);
      userlog(LOG_DEBUG, "input: %s", (line ? line : "<null>"));
      if (line == NULL || strlen(line) == 0) {
        return false;
      }
      else if (strcmp(line, "#") == 0) {
        break;
      }
      else {
        if (line[0] == '|')  line++;  // flat roots will be differentiated later

        int l = strlen(line);
        if (l > 1 && line[l-1] == '/')  line[l-1] = '\0';

        CHECK_NULL(array_push(new_roots, strdup(line)));
      }
    }

    return update_roots(new_roots);

  }

  return true;
}


static bool update_roots(array* new_roots) {
  userlog(LOG_INFO, "updating roots (curr:%d, new:%d)", array_size(ROOTS), array_size(new_roots));

  unregister_roots();
  if (array_size(new_roots) == 0) {
    return true;
  }
  else if (array_size(new_roots) == 1 && strcmp(array_get(new_roots, 0), "/") == 0) {  // refuse to watch entire tree
    output("UNWATCHEABLE\n/\n#\n");
    userlog(LOG_INFO, "unwatchable: /");
    array_delete_vs_data(new_roots);
    return true;
  }

  UNWATCHABLE = array_create(20);
  CHECK_NULL(UNWATCHABLE);
  if (!unwatchable_mounts(UNWATCHABLE)) {
    return false;
  }

  if (!register_roots(new_roots, UNWATCHABLE)) {
    return false;
  }

  // todo: sort/optimize list
  output("UNWATCHEABLE\n");
  for (int i=0; i<array_size(UNWATCHABLE); i++) {
    char* s = array_get(UNWATCHABLE, i);
    output("%s\n", s);
    userlog(LOG_INFO, "unwatchable: %s", s);
  }
  output("#\n");

  array_delete_vs_data(UNWATCHABLE);
  array_delete(new_roots);

  return true;
}


static void unregister_roots() {
  watch_node* root;
  while ((root = array_pop(ROOTS)) != NULL) {
    userlog(LOG_INFO, "unregistering root: %s", root->name);
    unwatch(root->wd);
    free(root->name);
    free(root);
  };
}


static bool register_roots(array* new_roots, array* unwatchable) {
  for (int i=0; i<array_size(new_roots); i++) {
    char* new_root = array_get(new_roots, i);
	watch_node* root = calloc(1,sizeof(watch_node));
    userlog(LOG_INFO, "registering root: %s", new_root);
    int id = watch(new_root, root, unwatchable);
    if (id == ERR_ABORT) {
      return false;
    } else if (id >= 0) {
      CHECK_NULL(array_push(ROOTS, root));
    } else {
      if (show_warning && watch_limit_reached()) {
        int limit = get_watch_count();
        userlog(LOG_WARNING, "watch limit (%d) reached", limit);
        //output("MESSAGE\n" INOTIFY_LIMIT_MSG, limit);
        show_warning = false;  // warn only once
      }
      CHECK_NULL(array_push(unwatchable, new_root));
    }
  }

  return true;
}

static bool is_watchable(const char* dev, const char* mnt, const char* fs, int local) {
#ifdef DEBUG
	userlog(LOG_DEBUG,"is_watchable: dev=%s, mnt=%s, fs=%s, local=%d",
			dev, mnt, fs, local);
#endif /* DEBUG */
	if(local == 0) 
		return false;
  // don't watch special and network filesystems
  return !(strncmp(mnt,"/tmp",4) == 0 || strncmp(mnt, "/dev", 4) == 0 || strncmp(mnt, "/proc", 5) == 0 || strncmp(mnt, "/sys", 4) == 0 ||
           strcmp(fs, "fuse.gvfs-fuse-daemon") == 0 || strcmp(fs, "cifs") == 0 || strcmp(fs, "nfs") == 0);
}

#define MTAB_DELIMS " \t"

static bool unwatchable_mounts(array* mounts) {
	struct statfs* mnt_points;
	int len;
	if((len = getmntinfo(&mnt_points,MNT_NOWAIT))==-1) {
		perror("getmntinfo");
	}
	for(int i=0;i<len;++i) {
		char* dev = mnt_points[i].f_mntfromname;
		char* point = mnt_points[i].f_mntonname;
		char* fs = mnt_points[i].f_fstypename;
		int local = (mnt_points[i].f_flags & MNT_LOCAL) != 0;
    if (!is_watchable(dev, point, fs, local)) {
      CHECK_NULL(array_push(mounts, strdup(point)));
    }
  }
  return true;
}

static void inotify_callback(char* path, int fflags) 
{

	if((fflags & NOTE_EXTEND) || (fflags & NOTE_WRITE)) {
		output("CHANGE\n%s\n",path);
    	userlog(LOG_DEBUG, "CHANGE:%s",path);
	}
	
	if(fflags & NOTE_ATTRIB) {
		output("STATS\n%s\n",path);
    	userlog(LOG_DEBUG, "STATS:%s",path);
	}

	if((fflags & NOTE_DELETE) || (fflags & NOTE_RENAME)) {
		output("DELETE\n%s\n",path);
    	userlog(LOG_DEBUG, "DELETE:%s",path);
	}

	if((fflags & NOTE_REVOKE)) {
		output("RESET\n%s\n",path);
    	userlog(LOG_DEBUG, "RESET:%s",path);
	}

}

void output(const char* format, ...) {
#ifdef DEBUG
  if (self_test) {
    return;
  }
#endif /* defined DEBUG */

  va_list ap;
  va_start(ap, format);
  vprintf(format, ap);
  va_end(ap);
}
