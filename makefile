
INCLUDE = -I/usr/local/include/ruby-2.1.0 \
					-I/usr/local/include/ruby-2.1.0/x86_64-linux	\
					-I/usr/include
				
LIB_PATH = -L/usr/local/lib -L/home/vincent/Documents/fsnet
				
					
OBJECTS = game.o

LDFLAG = 

MAKE_CMD = gcc

TARGET_NAME = game

game : main.c
	$(MAKE_CMD) $< $(LIB_PATH) -o $(TARGET_NAME) $(LDFLAG)  -lfsnet


clean:
	rm -rf $(OBJECTS)
