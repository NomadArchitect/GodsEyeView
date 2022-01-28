#ifndef UNISTD_HPP
#define UNISTD_HPP

void _exit(int status);
void _shutdown();
void _reboot();
int close(int descriptor);
int read(int descriptor, void* buffer, int length);
int write(int descriptor, char* buffer, int length);
int open(char* file_name);
void sleep(int sec);
int spawn(char* file_name, char** args);
int waitpid(int pid);
int listdir(char* dirname, char** entries);

#endif
