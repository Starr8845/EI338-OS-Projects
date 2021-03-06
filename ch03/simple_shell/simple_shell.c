#include <sys/types.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>

#define MAX_LINE 80

#define FLAG_DEFAULT 0			// 00000
#define FLAG_ERROR 1			// 00001
#define FLAG_AMPERSAND 2		// 00010
#define FLAG_LAST_COMMAND 4		// 00100
#define FLAG_PIPE 8				// 01000

#define READ_END 0
#define WRITE_END 1

void init_args(char **args)
{
	int i;
	for(i = 0; i < MAX_LINE/2; i++)
	{
		args[i] = NULL;
	}
}

void get_input(char *cmd)
{
	fgets(cmd, MAX_LINE, stdin);
//	printf("%d\n", strlen(cmd));
	if(strlen(cmd) > 1 && cmd[strlen(cmd)-1] == '\n')
	{
		cmd[strlen(cmd)-1] = '\0';
	}
}

int parse_input(char *cmd, char **args, char *infile, char *outfile, int *pipe_sep)
{
	char *token;
	int i = 0;
	int flag = FLAG_DEFAULT;
	int red = 0;
	int pipe_cnt = 0;
	
	token = strtok(cmd, " ");
	while(token != NULL)
	{
		if(strlen(token) <= 0)
		{
			token = strtok(NULL, " ");
			continue;
		}
		if(red)
		{
			switch(red)
			{
			case '<':
				strcpy(infile, token);
				infile[strlen(token)] = '\0';
				break;
			case '>':
				strcpy(outfile, token);
				outfile[strlen(token)] = '\0';
				break;
			default:
				break;
			}
			red = 0;
		}
		else if(!strcmp(token, "&"))
		{
			flag = flag | FLAG_AMPERSAND;
		}
		else if(!strcmp(token, "<") || !strcmp(token, ">"))
		{
			red = token[0];
			if(!strcmp(token, "<") && pipe_cnt != 0)
			{
				printf("Input redirection can only be used in the FIRST command in the sequence.\n");
				flag = flag | FLAG_ERROR;
			}
		}
		else if(!strcmp(token, "|"))
		{
			if(strlen(outfile))
			{
				printf("Output redirection can only be used in the LAST command in the sequence.\n");
				flag = flag | FLAG_ERROR;
			}
		
			flag = flag | FLAG_PIPE;
			args[i] = NULL;
			pipe_sep[pipe_cnt] = ++i - (pipe_cnt > 0 ? pipe_sep[pipe_cnt-1] : 0);
			//printf("%d\n", pipe_sep[pipe_cnt]);
			pipe_cnt++;
		}
		else
		{
			args[i] = (char*)malloc(sizeof(char) * (strlen(token) + 1));
			strcpy(args[i], token);
			args[i][strlen(token)] = '\0';
			//printf("%s\n", args[i]);
			i++;
		}
		
		token = strtok(NULL, " ");
	}
	args[i] = NULL;
	pipe_sep[pipe_cnt] = -1;
	
	if(red)
	{
		printf("Empty file descriptor.\n");
		flag = flag | FLAG_ERROR;
	}
	if(i == 0 || args[0][0] == '\n')
	{
		flag = flag | FLAG_ERROR;
	}
	
	return flag;
}

void clear_args(char **args)
{
	int i = 0;
	while(args[i] != NULL)
	{
		free(args[i]);
		args[i] = NULL;
		i++;
	}
}

/*
	"com1 | com2" =>
	Parent -- fork -- wait A
			   |
			   |
			   Child A -- fork -- wait B -- exe(com2)
			 	  	   	   |
				  	       |
					       Child B -- exe(com1)

*/

// pipe_exe_fall execute piped commands in the reverse order, that previous command is executed in the child process of the process that execute the current commands in a fall fashion
int pipe_exe_fall(char **args, int *pipe_sep, int fd_red_in, int fd_red_out)
{
	int fd[2];
	pid_t pid;
	int child_err;
	int tmp_fd;
	char** tmp_args;

	if(pipe(fd) == -1)
	{
		fprintf(stderr, "Pipe Failed");
		return -1;
	}
	
	pid = fork();
	if(pid < 0)
	{
		fprintf(stderr, "Fork Failed");
		return -1;
	}

	if(pid > 0)
	{
		// redirections
		close(fd[WRITE_END]);
		dup2(fd[READ_END], STDIN_FILENO);
		close(fd[READ_END]);
		if(fd_red_out != -1)
		{
			dup2(fd_red_out, STDOUT_FILENO);
		}
		
		// wait for child process before execute
		waitpid(pid, NULL, 0);

		// set the command and arguments to the correct ones
		tmp_args = args + pipe_sep[0];
		// execute
		child_err = execvp(tmp_args[0], tmp_args);
		if(child_err < 0)
		{
			printf("No command '%s' found.\n", tmp_args[0]);
			exit(0);
		}
	}
	else
	{
		// redirection
		close(fd[READ_END]);
		dup2(fd[WRITE_END], STDOUT_FILENO);
		close(fd[WRITE_END]);
		
		pipe_sep += 1;
		if(pipe_sep[1] == -1)
		{
			// only check input file redirection in the first command (bottom command in the fall)
			if(fd_red_in != -1)
			{
				dup2(fd_red_in, STDIN_FILENO);
			}
		
			// set the command and arguments to the correct ones
			tmp_args = args + pipe_sep[0];
			// execute
			child_err = execvp(tmp_args[0], tmp_args);
			if(child_err < 0)
			{
				printf("No command '%s' found.\n", tmp_args[0]);
				exit(0);
			}
		}
		else
		{
			// pipe execute remaining commands
			// the fd_red_out is set to one because only check output file redirection in the last command (top command in the fall)
			pipe_exe_fall(args, pipe_sep, fd_red_in, -1);
		}
	}
	exit(0);
}

// adjust pipe separators for pipe_fall
int adjust_fall(int *pipe_sep)
{
	int i = 0;
	int n;
	int tmp;
	while(pipe_sep[i+1] != -1)
	{
		pipe_sep[i+1] += pipe_sep[i];
		i++;
	}
	n = i;
	i = 0;
	pipe_sep[n+1] = 0;
	pipe_sep[n+2] = -1;
	// reverse pipe separator
	// the i-th separator points to the (total_command - i)-th command
	while(i < n)
	{
		tmp = pipe_sep[i];
		pipe_sep[i] = pipe_sep[n];
		pipe_sep[n] = tmp;
		n--;i++;
	}
}

/*
	"com1 | com2" =>
	Parent -- fork ---- fork -- wait all
			   |         |
			   |  		 |
			   Child A   Child B
			   |	     |
			   exe(com1) exe(com2)
*/

// pipe_exe_chain execute commands in multiple child processes that belong to the same parent process, which execute in a chained fashion
// FIXME: it will not work currently because pipe() creates ordinary pipe, we need named pipe
int pipe_exe_chain(char **args, int *pipe_sep, int fd_red_in, int fd_red_out)
{
	size_t children[MAX_LINE];
	int fd[MAX_LINE][2];
	int child_idx = 0;
	int err_flag = 0;
	int i;

	while(1)
	{
		// only establish pipe to next child if there exists a next command
		if(pipe_sep[0] != -1 && pipe(fd[child_idx]) == -1){
			fprintf(stderr, "Pipe Failed");
			err_flag = 1;
			break;
		}
		
		// fork child process
		children[child_idx] = fork();
		if(children[child_idx] < 0)
		{
			fprintf(stderr, "Fork Failed");
			err_flag = 1;
			break;
		}
		
		if(children[child_idx] == 0)
		{
			// the last child only checks output file redirection
			if(pipe_sep[0] == -1)
			{
				if(fd_red_out != -1)
				{
					dup2(fd_red_out, STDOUT_FILENO);
				}
			}
			// other children establish next pipe redirection
			// secure: pipe only establish when it is not the last child
			else
			{
				close(fd[child_idx][READ_END]);
				dup2(fd[child_idx][WRITE_END], STDOUT_FILENO);
				close(fd[child_idx][WRITE_END]);
			}
			// the first child only checks input file redirection
			if(child_idx == 0)
			{
				if(fd_red_in != -1)
				{
					dup2(fd_red_in, STDIN_FILENO);
				}
			}
			// other children establish previous pipe redirection
			else
			{
				close(fd[child_idx-1][WRITE_END]);
				dup2(fd[child_idx-1][READ_END], STDIN_FILENO);
				close(fd[child_idx-1][READ_END]);
			}
			// execute command
			int child_err = execvp(args[0], args);
			if(child_err < 0)
			{
				printf("No command '%s' found.\n", args[0]);
				exit(0);
			}
			exit(0);
		}
		// update child index and command information
		child_idx++;
		if(pipe_sep[0] == -1)
		{
			break;
		}
		args += pipe_sep[0];
		pipe_sep++;
	}
	// wait for termination of all children once
	for(i=0; i<child_idx; i++)
	{
		waitpid(children[i], NULL, 0);
	}
	exit(err_flag);
}

int main(void)
{
	char *args[MAX_LINE/2 + 1];
	int should_run = 1;
	int flag, last_flag = 0;
	int child_err = 0;
	char cmd[MAX_LINE];
	char cmd_buf[MAX_LINE+1];
	char infile[MAX_LINE];
	char outfile[MAX_LINE];
	int fd_in, fd_out;
	int dup_in, dup_out;
	int pipe_sep[MAX_LINE];
	
	pid_t pid;
	
	init_args(args);
	cmd_buf[0] = '\0';
	
	while(should_run)
	{
		printf("osh>");
		fflush(stdout);
		
		clear_args(args);
		infile[0] = '\0';
		outfile[0] = '\0';
		fd_in = -1;
		fd_out = -1;
		
		get_input(cmd);
		
		// history feature
		if(!strcmp(cmd, "!!"))
		{
			if(strlen(cmd_buf) <= 0)
			{
				printf("No commands in history.\n");
				continue;
			}
			strcpy(cmd, cmd_buf);
		}
		strcpy(cmd_buf, cmd);
		
		// parse command
		flag = parse_input(cmd, args, infile, outfile, pipe_sep);
		if(flag & FLAG_ERROR)
		{
			continue;
		}
		
		// check exit condition
		if(!strcmp(args[0], "exit"))
		{
			clear_args(args);
			break;
		}
		
		// open redirection files
		if(strlen(infile))
		{
			if((fd_in = open(infile, O_RDWR, 0644)) == -1)
			{
				printf("Open Error.\n");
				continue;
			}
		}
		if(strlen(outfile))
		{
			if((fd_out = open(outfile, O_RDWR|O_CREAT, 0644)) == -1)
			{
				printf("Open Error.\n");
				continue;
			}
		}

		// (1) fork a child process using fork()
		pid = fork();
		if(pid < 0)
		{
			fprintf(stderr, "Fork Failed");
			clear_args(args);	// TODO: should we clear memory when fork fails???
			return 1;
		}
		// (2) the child process will invoke execvp()
		else if(pid == 0)
		{
			// pipe execution is separated from regular execution
			// chained pipe may perform regular execution
			if(flag & FLAG_PIPE)
			{
				// two steps for pipe fall execution
				adjust_fall(pipe_sep);
				pipe_exe_fall(args, pipe_sep, fd_in, fd_out);

				// pipe chained execution
				// pipe_exe_chain(args, pipe_sep, infile, outfile);
			}
			// regular execution
			else
			{
				// redirection
				if(fd_in != -1)
				{
					if((dup_in = dup2(fd_in, STDIN_FILENO)) == -1)
					{
						printf("Dup2 Error.\n");
						close(fd_in);
						exit(0);
						continue;
					}
				}
				if(fd_out != -1)
				{
					if((dup_out = dup2(fd_out, STDOUT_FILENO)) == -1)
					{
						printf("Dup2 Error.\n");
						close(fd_out);
						exit(0);
						continue;
					}
				}

				// FIXME: although concurrent running is available,
				// still need to recheck the correctness of file operaiton
				if(!(flag & FLAG_AMPERSAND) || 0 == fork())
				{
					child_err = execvp(args[0], args);
					if(child_err < 0)
					{
						printf("No command '%s' found.\n", args[0]);
						exit(0);
					}
				}
				
				should_run = 0;
			}
		}
		// parent wait for the child process
		else
		{
			waitpid(pid, NULL, 0);
			
			// close file redirection
			if(fd_in != -1)
			{
				close(fd_in);
			}
			if(fd_out != -1)
			{
				close(fd_out);
			}
		}
	}
	
	return 0;
}

