#include <errno.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#include <chcore/syscall.h>
#include <chcore/memory.h>
#include <chcore-internal/procmgr_defs.h>
#include <chcore-internal/fs_defs.h>
#include <chcore/string.h>

#include "buildin_cmd.h"
#include "job_control.h"

int fsm_scan_pmo_cap = -1;
void *fsm_scan_buf = NULL;

bool waitchild = false;
struct list_head history_cmd_head;

struct history_cmd_node *history_cmd_pointer = NULL;

static int history_cmd_count = 0;

void init_buildin_cmd(void)
{
	init_list_head(&history_cmd_head);
}

/* Retrieve the entry name from one dirent */
static void get_dent_name(struct dirent *p, char name[])
{
	unsigned long len;
	len = p->d_reclen - sizeof(p->d_ino) - sizeof(p->d_off)
	      - sizeof(p->d_reclen) - sizeof(p->d_type);
	memcpy(name, p->d_name, len);
	name[len - 1] = '\0';
}

int do_complement(char *buf, char *complement, int complement_time)
{
	int ret = 0, j = 0, count = 0;
	struct dirent *p;
	char name[BUFLEN];
	char scan_buf[BUFLEN];
	int r = -1;
	int offset;
	char *matches[256] = {0}; /* Store all matching entries */
	size_t buf_len = strlen(buf);
	char buf_lower[BUFLEN]; /* For case-insensitive comparison */

	/* Convert buf to lowercase for case-insensitive matching */
	for (size_t i = 0; i < buf_len; i++) {
		buf_lower[i] = tolower(buf[i]);
	}
	buf_lower[buf_len] = '\0';

	/* XXX: only support '/' here */
	int root_fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (root_fd < 0) {
		return -1;
	}

	/* First pass: collect all matches */
	do {
		ret = getdents(root_fd, (struct dirent *)scan_buf, BUFLEN);
		if (ret <= 0) {
			break;
		}

		for (offset = 0; offset < ret; offset += p->d_reclen) {
			p = (struct dirent *)(scan_buf + offset);
			get_dent_name(p, name);
			
			/* Create lowercase version of name for comparison */
			char name_lower[BUFLEN];
			size_t name_len = strlen(name);
			for (size_t i = 0; i < name_len; i++) {
				name_lower[i] = tolower(name[i]);
			}
			name_lower[name_len] = '\0';
			
			/* Match at the beginning like bash, but case-insensitive */
			if (strncmp(name_lower, buf_lower, buf_len) == 0) {
				if (count < 256) {
					matches[count] = strdup(name);
					count++;
				}
			}
		}
	} while (ret > 0);

	/* If we have matches */
	if (count > 0) {
		/* Sort matches alphabetically */
		for (int i = 0; i < count - 1; i++) {
			for (int k = i + 1; k < count; k++) {
				if (strcmp(matches[i], matches[k]) > 0) {
					char *temp = matches[i];
					matches[i] = matches[k];
					matches[k] = temp;
				}
			}
		}

		/* Get the match based on complement_time (tab press count) */
		int match_index = complement_time % count;
		strlcpy(complement, matches[match_index], BUFLEN);
		r = 0;

		/* Free allocated memory */
		for (int i = 0; i < count; i++) {
			free(matches[i]);
		}
	}

	close(root_fd);
	return r;
}

int do_cd(char *cmdline)
{
	cmdline += 2;
	while (*cmdline == ' ')
		cmdline++;
	if (*cmdline == '\0')
		return 0;
	if (*cmdline != '/') {
	}
	printf("Build-in command cd %s: Not implemented!\n", cmdline);
	return 0;
}

int do_top(void)
{
	usys_top();
	return 0;
}

int do_ls(char *cmdline)
{
	struct dirent *p;
	char pathbuf[BUFLEN];
	char scan_buf[BUFLEN];
	char name[BUFLEN];
	int offset;
	int readbytes;
	int show_all = 0;
	int show_long = 0;
	int show_color = 1;
	int i;
	char *args[NR_ARGS_MAX];
	int arg_count = 0;

	pathbuf[0] = '\0';
	cmdline += 2;
	while (*cmdline == ' ')
		cmdline++;

	/* Parse arguments */
	char *token = strtok(cmdline, " ");
	while (token != NULL && arg_count < NR_ARGS_MAX) {
		args[arg_count++] = token;
		token = strtok(NULL, " ");
	}

	/* Process options */
	for (i = 0; i < arg_count; i++) {
		if (args[i][0] == '-') {
			char *opt = args[i] + 1;
			while (*opt) {
				switch (*opt) {
				case 'a':
					show_all = 1;
					break;
				case 'l':
					show_long = 1;
					break;
				case 'G':
					show_color = 1;
					break;
				default:
					printf("ls: invalid option -- '%c'\n", *opt);
					break;
				}
				opt++;
			}
			/* Mark this as an option so we don't treat it as a path */
			args[i] = NULL;
		}
	}

	/* Find the path argument (if any) */
	char *path = NULL;
	for (i = 0; i < arg_count; i++) {
		if (args[i] != NULL) {
			path = args[i];
			break;
		}
	}

	if (path == NULL) {
		/* No path specified, use current directory */
		getcwd(pathbuf, BUFLEN);
	} else {
		/* Use specified path */
		strlcat(pathbuf, path, BUFLEN);
	}

	/* Try open `pathbuf` as dir */
	int dirfd = open(pathbuf, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dirfd < 0) {
		/* Error Handling */
		if (dirfd == -ENOTDIR) {
			printf("`%s` is not a directory\n", pathbuf);
		} else {
			printf("ls: cannot access '%s': %s\n", pathbuf, strerror(-dirfd));
		}
		return dirfd;
	}

	/* Store entries for sorting and formatting */
	char entries[256][BUFLEN];
	int entry_count = 0;

	do {
		readbytes = getdents(dirfd, (struct dirent *)scan_buf, BUFLEN);
		for (offset = 0; offset < readbytes; offset += p->d_reclen) {
			p = (struct dirent *)(scan_buf + offset);
			get_dent_name(p, name);
			
			/* Skip hidden files unless -a is specified */
			if (!show_all && name[0] == '.')
				continue;
				
			strlcpy(entries[entry_count], name, BUFLEN);
			entry_count++;
			
			if (entry_count >= 256) {
				printf("ls: too many entries to display\n");
				break;
			}
		}
	} while (readbytes > 0 && entry_count < 256);
	
	close(dirfd);

	/* Sort entries alphabetically */
	for (i = 0; i < entry_count - 1; i++) {
		for (int j = i + 1; j < entry_count; j++) {
			if (strcmp(entries[i], entries[j]) > 0) {
				char temp[BUFLEN];
				strlcpy(temp, entries[i], BUFLEN);
				strlcpy(entries[i], entries[j], BUFLEN);
				strlcpy(entries[j], temp, BUFLEN);
			}
		}
	}

	/* Display entries */
	if (show_long) {
		/* Long format display */
		for (i = 0; i < entry_count; i++) {
			/* In a real implementation, we would show permissions, size, etc. */
			printf("drwxr-xr-x 1 root root 4096 Jan 1 00:00 %s\n", entries[i]);
		}
	} else {
		/* Simple format display */
		int max_width = 0;
		for (i = 0; i < entry_count; i++) {
			int len = strlen(entries[i]);
			if (len > max_width)
				max_width = len;
		}
		
		max_width += 2; /* Add spacing between columns */
		int cols = 160 / max_width; /* Assume 80 column terminal */
		if (cols < 1) cols = 1;
		
		for (i = 0; i < entry_count; i++) {
			if (show_color) {
				/* Check if it's a directory (simplified) */
				char test_path[BUFLEN * 2];
				snprintf(test_path, BUFLEN * 2, "%s/%s", pathbuf, entries[i]);
				int test_fd = open(test_path, O_RDONLY | O_DIRECTORY);
				if (test_fd >= 0) {
					/* Directory - blue */
					printf("\033[1;34m%-*s\033[0m", max_width, entries[i]);
					close(test_fd);
				} else {
					/* Check file extensions for different colors */
					char *ext = strrchr(entries[i], '.');
					if (ext) {
						if (strcmp(ext, ".bin") == 0) {
							/* Binary files - green */
							printf("\033[1;32m%-*s\033[0m", max_width, entries[i]);
						} else if (strcmp(ext, ".so") == 0 || strstr(entries[i], ".so.") != NULL) {
							/* Shared objects - yellow */
							printf("\033[1;33m%-*s\033[0m", max_width, entries[i]);
						} else if (strcmp(ext, ".txt") == 0) {
							/* Text files - cyan */
							printf("\033[1;36m%-*s\033[0m", max_width, entries[i]);
						} else if (strcmp(ext, ".json") == 0) {
							/* JSON files - cyan */
							printf("\033[1;36m%-*s\033[0m", max_width, entries[i]);
						} else {
							/* Regular files */
							printf("%-*s", max_width, entries[i]);
						}
					} else {
						/* No extension */
						printf("\033[1;32m%-*s\033[0m", max_width, entries[i]);
					}
				}
			} else {
				printf("%-*s", max_width, entries[i]);
			}
			
			if ((i + 1) % cols == 0 || i == entry_count - 1)
				printf("\n");
		}
		if (entry_count % cols != 0)
			printf("\n");
	}

	return 0;
}

void do_clear(void)
{
	usys_putc(12);
	usys_putc(27);
	usys_putc('[');
	usys_putc('2');
	usys_putc('J');
}

/* Show the all jobs in the bg_jobs array. */
void do_jobs(void)
{
	long count = 0;
	int i;
	int total_job;

	pthread_mutex_lock(&job_mutex);
	total_job = job_count;
	/*
	 * Iterate through the bg_jobs array to update the state of child
	 * processes. If a child process has exited already, we need del it from
	 * the bg_jobs array.
	 */
	for (i = 0; i < total_job;) {
		if (bg_jobs[count] != NULL) {
			if (!check_job_state(bg_jobs[count]->pid)) {
				printf("[%ld] + %d %s\n",
				       count + 1,
				       bg_jobs[count]->pid,
				       bg_jobs[count]->job_name);
			} else {
				del_job(count);
			}
			i++;
		}
		count++;
	}
	pthread_mutex_unlock(&job_mutex);
}

int do_fg(char *cmdline)
{
	long int index = 0;
	char input_string[NR_ARGS_MAX] = {0};
	char *stop_string;
	int i = 0;
	int pid;
	int ret = -1;

	cmdline += strlen("fg ");

	while (cmdline[i] != '\0' && cmdline[i] != ' ') {
		input_string[i] = cmdline[i];
		i++;
		/* The string length exceeds the buffer length. */
		if (i == NR_ARGS_MAX - 1) {
			goto out;
		}
	}

	/*
	 * NOTE: If the string contains non-numeric elements, the stop_string will
	 * not equal '\0'.
	 */
	index = strtol(input_string, &stop_string, 10) - 1;
	if (*stop_string != '\0') {
		goto out;
	}
	if (index < 0 || index > JOBS_MAX - 1) {
		goto out;
	}

	pthread_mutex_lock(&job_mutex);
	if (bg_jobs[index] == NULL) {
		goto unlock;
	}

	if (!check_job_state(bg_jobs[index]->pid)) {
		waitchild = true;
		printf("[%ld] - %d continued %s\n",
		       index + 1,
		       bg_jobs[index]->pid,
		       bg_jobs[index]->job_name);
		pid = bg_jobs[index]->pid;
		if (bg_jobs[index]->pmo_cap > 0) {
			foreground_pmo_addr =
				chcore_auto_map_pmo(bg_jobs[index]->pmo_cap,
						    PAGE_SIZE,
						    PROT_READ | PROT_WRITE);
		}

		fg_job.pmo_cap = bg_jobs[index]->pmo_cap;
		foreground_buffer_addr = foreground_pmo_addr + sizeof(u32) * 2;
		strlcpy(fg_job.job_name,
		        bg_jobs[index]->job_name,
		        sizeof(fg_job.job_name));
		fg_job.pid = pid;
		fg_job.notific_cap = bg_jobs[index]->notific_cap;
		del_job(index);
		ret = pid;
	}

unlock:
	pthread_mutex_unlock(&job_mutex);

out:
	if (ret == -1) {
		printf("fg: no such job in background\n");
	}
	return ret;
}

/* Free the oldest record. */
static void free_history_record(void)
{
	struct history_cmd_node *temp;

	if (!list_empty(&history_cmd_head)) {
		temp = container_of(history_cmd_head.prev,
				    struct history_cmd_node,
				    cmd_node);
		list_del(&temp->cmd_node);
		free(temp->cmd);
		free(temp);
	}
}

void add_cmd_to_history(char *cmd)
{
	struct history_cmd_node *cmd_node;
	int cmd_buffer_size;

	cmd_buffer_size = MIN(BUFLEN, strlen(cmd) + 1);

	if (strlen(cmd) != 0) {
		cmd_node = (struct history_cmd_node *)malloc(
			sizeof(struct history_cmd_node));
		cmd_node->cmd = (char *)malloc(cmd_buffer_size);
		cmd_node->index = history_cmd_count + 1;
		strlcpy(cmd_node->cmd, cmd, cmd_buffer_size);
		list_add(&cmd_node->cmd_node, &history_cmd_head);
		history_cmd_count++;
		if (history_cmd_count > MAX_HISTORY_CMD_RECORD) {
			free_history_record();
		}
	}
}

/*
 * TODO: We save record in memory now. We can write this record to disk in the
 * future.
 */
void do_history(void)
{
	struct history_cmd_node *cmd;

	for_each_in_list_reverse (
		cmd, struct history_cmd_node, cmd_node, &history_cmd_head) {
		printf("%4d  %s\n", cmd->index, cmd->cmd);
	}
}

bool do_up(void)
{
	if (history_cmd_pointer == NULL) {
		if (list_empty(&history_cmd_head)) {
			return false;
		}
		history_cmd_pointer = container_of(history_cmd_head.next,
						   struct history_cmd_node,
						   cmd_node);
	} else if (history_cmd_pointer->cmd_node.next != &history_cmd_head) {
		history_cmd_pointer =
			container_of(history_cmd_pointer->cmd_node.next,
				     struct history_cmd_node,
				     cmd_node);
	} else {
		return false;
	}
	return true;
}

bool do_down(void)
{
	if (history_cmd_pointer == NULL) {
		return false;
	}
	if (history_cmd_pointer->cmd_node.prev != &history_cmd_head) {
		history_cmd_pointer =
			container_of(history_cmd_pointer->cmd_node.prev,
				     struct history_cmd_node,
				     cmd_node);
		return true;
	}
	return false;
}

void clear_history_point(void)
{
	history_cmd_pointer = NULL;
}

int do_source(char *cmdline)
{
	char script_path[BUFLEN];
	FILE *fp;
	char line[BUFLEN];
	int ret = 0;

	/* Skip the 'source' command and any spaces */
	cmdline += 6;
	while (*cmdline == ' ')
		cmdline++;

	if (*cmdline == '\0') {
		printf("source: missing file operand\n");
		return -1;
	}

	/* Get the script path */
	strlcpy(script_path, cmdline, BUFLEN);

	/* Open the script file */
	fp = fopen(script_path, "r");
	if (fp == NULL) {
		printf("source: cannot open '%s': %s\n", script_path, strerror(errno));
		return -1;
	}

	/* Read and execute each line */
	while (fgets(line, BUFLEN, fp) != NULL) {
		/* Remove trailing newline */
		line[strcspn(line, "\n")] = 0;

		/* Skip empty lines and comments */
		if (line[0] == '\0' || line[0] == '#')
			continue;

		/* Execute the command */
		if ((ret = builtin_cmd(line)) != 0) {
			if (ret < 0) {
				printf("Error executing command: %s\n", line);
				break;
			}
			continue;
		}
		if ((ret = run_cmd(line)) < 0) {
			printf("Cannot run %s, ERROR %d\n", line, ret);
			break;
		}
	}

	fclose(fp);
	return ret;
}
