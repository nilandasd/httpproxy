
EXECBIN   = httpproxy
COMPILECPP = gcc httpproxy.c -o httpproxy -pthread -Wall -Wextra -Wpedantic -Wshadow

all : spotless ${EXECBIN}
	- ls ..
	- rm ../check_submission
	- cp check_submission ../

${EXECBIN} :
	${COMPILECPP}

clean :
	- rm *.o

spotless : clean
	- rm ${EXECBIN}
