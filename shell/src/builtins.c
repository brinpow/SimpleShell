#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <limits.h>

#include <sys/types.h>

#include "builtins.h"

#define CONVERT_ERROR 10

int echo(char*[]);
int undefined(char *[]);
int lexit(char*[]);
int lcd(char*[]);
int lls(char*[]);
int lkill(char*[]);

builtin_pair builtins_table[]={
    {"exit",    &lexit},
    {"lecho",   &echo},
    {"cd",      &lcd},
    {"lcd",     &lcd},
    {"lkill",   &lkill},
    {"lls",     &lls},
    {NULL,NULL}
};

int arg_number(char * argv[])
{
    int i=0;
    while(argv[i]!=NULL)
    {
        i++;
    }
    return i;
}
int 
echo( char * argv[])
{
	int i =1;
	if (argv[i]) printf("%s", argv[i++]);
	while  (argv[i])
		printf(" %s", argv[i++]);

	printf("\n");
	fflush(stdout);
	return 0;
}

int lexit(char* argv[])
{
	int n=arg_number(argv);
	if(n==1)
	{
		exit(EXIT_SUCCESS);
		return -1;
	}
	else
	{
		return -1;
	}
}

int lcd(char* argv[])
{	
	int n=arg_number(argv);
	if(n==2)
	{
		return chdir(argv[1]);
	}
	else if(n==1)
	{
		return chdir(getenv("HOME"));
	}
	else
	{
		return -1;
	}
}

int lls(char* argv[])
{
	int n=arg_number(argv);
	if(n==1)
	{
		DIR * curr_dir = opendir(".");
		errno=0;
		if(curr_dir==NULL && errno)
		{
			return -1;
		}
		while(1)
		{
			errno=0;
			struct dirent * curr_dirent = readdir(curr_dir);
			if(curr_dirent==NULL && errno)
			{
				return -1;
			}
			else if(curr_dirent!=NULL)
			{
				if(curr_dirent->d_name[0]!='.')
				{
					printf("%s\n",curr_dirent->d_name);
					fflush(stdout);
				}
			}
			else
			{
				break;
			}
		}
		int rv;
		do
		{
			rv=closedir(curr_dir);
		} while (rv==-1 && errno==EINTR);
		
		return rv;
		
	}
	else
	{
		return -1;
	}
}

int convert(char * arg)
{
	char* end;
	errno = 0;
	long result=strtol(arg,&end,10);
	if(arg==end||*end!='\0'||result>INT_MAX||result<INT_MIN||errno!=0)
	{
		errno=CONVERT_ERROR;
		return -1;
	}
	return result;
}

int lkill(char* argv[])
{
	int n=arg_number(argv);
	if(n==2)
	{
		int pid = convert(argv[1]);
		if(pid==-1 && errno==CONVERT_ERROR)
		{
			return -1;
		}
		return kill(pid,SIGTERM);
	}
	else if (n==3)
	{
		if(*argv[1]!='-')
		{
			return -1;
		}
		int sig = convert(argv[1]+1);
		if((sig==-1 && errno==CONVERT_ERROR))
		{
			return -1;
		}
		int pid = convert(argv[2]);
		if(pid==-1 && errno==CONVERT_ERROR)
		{
			return -1;
		}
		return kill(pid,sig);
	}
	else
	{
		return -1;
	}
}

int undefined(char * argv[])
{
	fprintf(stderr, "Command %s undefined.\n", argv[0]);
	return BUILTIN_ERROR;
}
