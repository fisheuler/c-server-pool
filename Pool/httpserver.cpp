#include<sys/types.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<assert.h>
#include<stdio.h>
#include<unistd.h>
#include<errno.h>
#include<string.h>
#include<fcntl.h>
#include<stdlib.h>
#include<sys/epoll.h>
#include<pthread.h>
#include<stdbool.h>
#include<libgen.h>



	enum CHECK_STATE { CHECK_STATE_REQUESTLINE = 0,
					   CHECK_STATE_HEADER,
					   CHECK_STATE_CONNECT
	};

	enum HTTP_CODE { NO_REQUEST,GET_REQUEST,BAD_REQUEST,NO_RESOURCE,FORBIDDEN_REQUEST,FILE_REQUEST,
					INTERNAL_ERROR,CLOSED_CONNECTION
	};

	enum LINE_STATUS { LINE_OK =0, LINE_BAD,LINE_OPEN};

	static const char* szret[] = {"I get a correct result\n","something wrong\n"};


#define MAX_EVENT_NUMBER 1024
#define BUFFER_SIZE 10

int setnonblocking(int fd)
{
	int old_option = fcntl(fd,F_GETFL);
	int new_option = old_option | O_NONBLOCK;
	fcntl(fd,F_SETFL,new_option);
	return old_option;
}


void addfd(int epollfd,int fd, bool enable_et)
{
	struct epoll_event event;
	event.data.fd = fd;
	event.events = EPOLLIN;
	if(enable_et)
	{
		event.events |= EPOLLET;
	}
	
	epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);

	setnonblocking(fd);
}


void lt(struct epoll_event* events,int number, int epollfd, int listenfd)
{
	char buf[BUFFER_SIZE];
	
	for( int i=0; i< number;i++)
	{
		int sockfd = events[i].data.fd;
		if(sockfd == listenfd)
		{
			struct sockaddr_in client_address;
			socklen_t client_addrlength = sizeof(client_address);
			
			int connfd = accept(listenfd,(struct sockaddr*) &client_address,&client_addrlength);

			addfd(epollfd,connfd,false);

		}
		else if (events[i].events & EPOLLIN)
		{
			printf("event trigger once\n");
			memset(buf,'\0',BUFFER_SIZE);
			int ret = recv(sockfd,buf,BUFFER_SIZE-1,0);
			if(ret <= 0)
			{
				close(sockfd);
				continue;
			}
			printf("get %d bytes of content: %s\n",ret, buf);
		}
		else {
			printf("something else happen\n");
		}

	}
}
				
void et(struct epoll_event* events, int number, int epollfd, int listenfd)
{
	char buf[BUFFER_SIZE];
	for(int i =0;i<number;i++)
	{
		int sockfd = events[i].data.fd;
		if(sockfd == listenfd)
		{
			struct sockaddr_in client_address;
			socklen_t client_addrlength = sizeof(client_address);
			int connfd = accept(listenfd,(struct sockaddr*) &client_address,&client_addrlength);
			addfd(epollfd,connfd,true);

		}
		else if( events[i].events & EPOLLIN)
		{
			printf("event trigger once\n");
			while(1)
			{
				memset(buf,'\0',BUFFER_SIZE);
				int ret = recv(sockfd,buf,BUFFER_SIZE-1,0);
				if(ret < 0)
				{
					// 对于非阻塞io，下面条件表示数据已经全部读取完毕
					// 此后，epoll就可以再次触发EPOLLIN事件
					if((errno == EAGAIN) || (errno == EWOULDBLOCK))
					{
						printf("read later\n");
						break;
					}
					
					close(sockfd);
					break;

				}
				else if( ret== 0)
				{
					close(sockfd);
					break;
				}
				else
				{
					printf("get %d bytes : %s\n",ret,buf);
				}

			}
		
		}
		else 
		{
			printf("something else happen\n");
		}
	}

}


LINE_STATUS parse_line(char* buffer,int& checked_index,int&read_index)
{
	char temp;

	/* read_index为缓冲区下一个要写入字符的下标
	 * 0- checked_index 已经分析
	 * checked_indx - read_index-1 为我们分析的对象
	 */

	for(; checked_index < read_index;++checked_index)
	{
		temp = buffer[checked_index];

		/*如果当前字段为"\r"，则说明可能读取到一个完整的行*/
		if( temp == "\r")
		{
			if((checked_index + 1) == read_index)
			{
				return LINE_OPEN;
			}
			else if (buffer[checked_index+1] == "\n")
			{
				/* 将“\r\n"转换为\0\0
				 *
				 */
				buffer[checked_index++]='\0';
				buffer[checked_index++]='\0';
				return LINE_OK;
			}
			/* 否则，客户端发送的http请求存在语法问题*/
			return LINE_BAD;
		}
		else if ( temp == "\n")
		{
			if( (checked_index > 1) && buffer[checked_index-1]=='\r')
			{
				buffer[checked_index-1]='\0';
				buffer[checked_index++]='\0';
				return LINE_OK;
			}

			return LINE_BAD;
		}

	}

	/* 遍历结束之后也没有遇到‘\r'字符，说明还得继续读取客户端数据*/
	return LINE_OPEN;
}

/*分析请求行*/

HTTP_CODE parse_requestline(char* temp,CHECK_STATE& checkstate)
{
	char* url = strpbrk(temp," \t");
	if( !url)
	{
		return BAD_REQUEST;
	}
	*url++='\0';

	char* method = temp;

	if(strcasecmp(method,"GET") == 0)
	{
		printf("The request method is GET\n");

	}
	else
	{
		return BAD_REQUEST;
	}

	url += strspn(url," \t");

	char* version = strpbrk(url," \t");

	if(!version)
	{
		return BAD_REQUEST;
	}

	*version++ = '\0';
	version += strspn(version," \t");

	if( strcasecmp(version,"HTTP/1.1") != 0)
	{
		return BAD_REQUEST;
	}

	if( strncasecmp(url,"http://",7) == 0)
	{
		url+=7;
		url = strchr(url,'/');
	}

	if( !url || url[0] != '/')
	{
		return BAD_REQUEST;
	}

	printf("The request URL is: %s\n",url);
	checkstate = CHECK_STATE_HEADER;
	return NO_REQUEST;
}


HTTP_CODE parse_headers(char* temp)
{
	/* 遇到空行，说明我们得到了一个正确的http请求*/

	if(temp[0] == '\0')
	{
		return GET_REQUEST;
	}
	else if( strncasecmp(temp,"Host:",5)==0	)
	{
		temp+=5;
		temp+= strspn(temp," \t");
		printf("the request host is:%s\n",temp);
	}
	else
	{
		printf("I can not handle this header\n");
	}

	return NO_REQUEST;

}


HTTP_CODE parse_content(char* buffer,int& checked_index,CHECK_STATE& checkstate,
int& read_index,int& start_line)
{
	LINE_STATUS linestatus = LINE_OK;
	HTTP_CODE retcode = NO_REQUEST;
	

	//主状态机，用于从buffer中取出所有完整的行

	while((linestatus = parse_line(buffer,checked_index,read_index)) == LINE_OK)
	{
		char* temp = buffer + start_line; /*start_line为buffer中的起始位置。*/
		start_line = checked_index;

		switch(checkstate)
		{
			case CHECK_STATE_REQUESTLINE: /*第一个状态，分析请求行*/
			{
				retcode = parse_requestline(temp,checkstate);
				if ( retcode == BAD_REQUEST)
				{
					return BAD_REQUEST;
				}
				break;
			}
			case CHECK_STATE_HEADER: /*第二个状态，分析头部*/
			{
				retcode = parse_headers(temp);
				if( retcode == BAD_REQUEST)
				{
					return BAD_REQUEST;
				}
				else if ( retcode == GET_REQUEST)
				{
					return GET_REQUEST;
				}
				break;
			}
			default:
			{
				return INTERNAL_ERROR;
			}
		}
	}

	/* 若没有读取到一个完整的行，标识还需要继续读取客户数据才能进一步分析*/

	if( linestatus == LINE_OPEN)
	{
		return NO_REQUEST;
	}
	else
	{
		return BAD_REQUEST;
	}

}


int main(int argc ,char* argv[])
{
	if(argc <= 2)
	{
		printf("usage: %s ip_address port_number \n",basename(argv[0]));
		return 1;
	}

	const char* ip = argv[1];
	int port = atoi(argv[2]);
	
	int ret= 0;
	
	struct sockaddr_in address;
	bzero(&address,sizeof(address));
	address.sin_family =AF_INET;
	inet_pton(AF_INET,ip,&address.sin_addr);
	address.sin_port = htons(port);

	int listenfd = socket(PF_INET,SOCK_STREAM,0);
	assert(ret != -1);

	int ret = bind(listenfd,(struct sockaddr*)&address,sizeof(address));
	assert( ret != -1);
	
	ret = listen(listenfd,5);
	assert(ret != -1);

	struct sockaddr_in client_address;

	socklen_t client_addrlength = sizeof(client_address);

	int fd = accept(listenfd,(struct sockaddr*)&client_address,&client_addrlength);

	if(fd <0)
	{
		printf("errno is: %d\n",errno);
	}
	else
	{
		char buffer[BUFFER_SIZE];
		memset(buffer,'\0',BUFFER_SIZE);

		int data_read = 0;
 		int read_index = 0;
		int checked_index =0;
		int start_line = 0;

		CHECK_STATE checkstate = CHECK_STATE_REQUESTLINE;

		while(1)
		{
			data_read = recv(fd,buffer+read_index,BUFFER_SIZE-read_index,0);

			if( data_read == -1)
			{
				printf("reading failed\n");
				break;
			}
			else if( data_read ==0 )
			{
				printf("remote client has closed the connection\n");
				break;
			}

			read_index += data_read;

			HTTP_CODE result = parse_content(buffer,checked_index,checkstate,read_index,start_line);

			if( result == NO_REQUEST)
			{
				continue;
			}
			else if ( result == GET_REQUEST)
			{
				send(fd,szret[0],strlen(szret[0]),0);
				break;
			}
			else
			{
				send(fd,szret[1],strlen(szret[1]),0);
				break;
			}

		}


		close(fd);

	}

	close(listenfd);

	return 0;

}
		
