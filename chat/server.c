/*
 * server.c
 *
 *  Created on: 2014-4-27
 *      Author: fisheuler
 */


#define _GNU_SOURCE 1
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
#include<poll.h>


#define USER_LIMIT 5
#define BUFFER_SIZE 64
#define FD_LIMIT 65535

typedef struct client_data{

	struct sockaddr_in address;

	char* write_buf;
	char buf[BUFFER_SIZE];

}client_data;

int setnonblocking(int fd){

	int old_option = fcntl(fd,F_GETFL);

	int new_option = old_option | O_NONBLOCK;

	fcntl(fd,F_SETFL,new_option);

	return old_option;

}


int main(int argc , char * argv[]){


	if(argc <= 2){

		printf("usage:%s ip_address port_number\n",basename(argv[0]));
		return 1;
	}

	const char* ip = argv[1];
	int port = atoi(argv[2]);

	int ret = 0;

	struct sockaddr_in server_address;

	bzero(&server_address,sizeof(server_address));

	server_address.sin_family = AF_INET;

	inet_pton(AF_INET,ip,&server_address.sin_addr);
	server_address.sin_port = htons(port);


	int listenfd = socket(PF_INET,SOCK_STREAM,0);

	assert(listenfd >= 0);

	ret = bind(listenfd,(struct sockaddr*) &server_address, sizeof(server_address));

	assert(ret != -1);


	ret = listen(listenfd,5);

	assert(ret != -1);

	//将socket和客户数据关联的方法，socket为数组下标
	client_data* users = (client_data *) malloc(sizeof(client_data)*FD_LIMIT);

	client_data users_1[FD_LIMIT];

	struct pollfd fds[USER_LIMIT + 1];

	int user_counter = 0;
	int i = 1;
	for( i ; i <= USER_LIMIT; ++i){

		fds[i].fd = -1;
		fds[i].events = 0;
	}

	fds[0].fd= listenfd;
	fds[0].events = POLLIN | POLLERR;
	fds[0].revents = 0;

	while(1){

		ret = poll(fds,user_counter+1,-1);

		if(ret < 0){
			printf("poll failuer\n");
			break;

		}

		int i =0;
		for(i; i< user_counter+1;++i){

			if(( fds[i].fd == listenfd) &&( fds[i].revents & POLLIN)){

				struct sockaddr_in client_address;
				socklen_t client_addrlength = sizeof(client_address);
				int connfd = accept(listenfd,(struct sockaddr*) &client_address,&client_addrlength);

				if( connfd < 0){

					printf("errno is: %d\n",errno);

					continue;
				}

				if(user_counter >= USER_LIMIT){
					const char* info ="too many users\n";
					printf("%s",info);
					send(connfd,info,strlen(info),0);

					close(connfd);
					continue;

				}

				user_counter++;
				users[connfd].address = client_address;
				setnonblocking(connfd);
				fds[user_counter].fd = connfd;
				fds[user_counter].events = POLLIN|POLLRDHUP|POLLERR;
				fds[user_counter].revents=0;

				printf("comes a new user,now hava %d users\n",user_counter);

			}

			else if( fds[i].revents & POLLERR){
				printf("get an error from %d\n",fds[i].fd);
				char errors[100];
				memset(errors,'\0',100);

				socklen_t length = sizeof(errors);
				if(getsockopt(fds[i].fd,SOL_SOCKET,SO_ERROR,&errors,&length)<0){
					printf("get socket option failed\n");
				}
				continue;
			}
			else if( fds[i].revents & POLLRDHUP){

				//如果客户端关闭链接，服务器也关闭链接，将用户数减1
				users[fds[i].fd] = users[fds[user_counter].fd];
				close(fds[i].fd);
				fds[i] = fds[user_counter];
				i--;
				user_counter--;
				printf("a client left\n");

			}
			else if( fds[i].revents & POLLIN){

				int connfd = fds[i].fd;

				memset(users[connfd].buf,'\0',BUFFER_SIZE);
				ret = recv(connfd,users[connfd].buf,BUFFER_SIZE-1,0);
				printf("get %d bytes of client data %s from %d\n",ret,users[connfd].buf,connfd);
				if(ret < 0){
					if(errno != EAGAIN){
						close(connfd);
						users[fds[i].fd] = users[fds[user_counter].fd];
						fds[i] = fds[user_counter];
						i--;
						user_counter--;

					}
				}
				else if( ret ==0){

				}else{
					// 通知其他客户端准备写数据
					for(int j = 1;j<= user_counter;++j){
						if(fds[j].fd == connfd){
							continue;
						}
						fds[j].events |= ~POLLIN;
						fds[j].events |= POLLOUT;
						users[fds[j].fd].write_buf = users[connfd].buf;
					}
				}
			}

			else if (fds[i].revents & POLLOUT){
				int connfd = fds[i].fd;
				if( ! users[connfd].write_buf){
					continue;
				}

				ret = send(connfd,users[connfd].write_buf,strlen(users[connfd].write_buf),0);

				users[connfd].write_buf = NULL;

				fds[i].events |= ~POLLOUT;
				fds[i].events |= POLLIN;


			}
		}
	}

	free(users);

	close(listenfd);

	return 0;































}
