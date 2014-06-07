
INCLUDE = -I/usr/local/include/ruby-2.1.0 \
	  -I/usr/local/include/ruby-2.1.0/x86_64-linux	\
	  -I/usr/include
				
LIB_PATH = -L/usr/local/lib
				
					
OBJECTS = fs_loop_queue.o fs_malloc.o fs_pack.o fs_node.o  fs_server.o fs_stream.o hash.o rb_define.o rb_main.o

LDFLAG = -fPIC

MAKE_CMD = gcc

TARGET_NAME = libfsnet.so

fsnet: $(OBJECTS)
	$(MAKE_CMD) $(LIB_PATH) -o $(TARGET_NAME) $(LDFLAG) -shared $(OBJECTS) -lruby -levent -levent_extra -levent_pthreads

fs_malloc.o : fs_malloc.c fs_malloc.h
	$(MAKE_CMD) -c $< $(INCLUDE) $(LDFLAG)
	
fs_node.o : fs_node.c fs_node.h
	$(MAKE_CMD) -c $< $(INCLUDE) $(LDFLAG)
		
fs_server.o : fs_server.c fs_server.h
	$(MAKE_CMD) -c $< $(INCLUDE) $(LDFLAG)
	
fs_stream.o : fs_stream.c fs_stream.h
	$(MAKE_CMD) -c $< $(INCLUDE) $(LDFLAG)

rb_define.o : ruby_define/rb_define.c ruby_define/rb_define.h
	$(MAKE_CMD) -c $< $(INCLUDE) $(LDFLAG)
	
rb_main.o : ruby_define/rb_main.c ruby_define/rb_define.h
	$(MAKE_CMD) -c $< $(INCLUDE) -std=c99 $(LDFLAG)

hash.o : hash.c hash.h
	$(MAKE_CMD) -c $< $(INCLUDE) $(LDFLAG)
	
fs_loop_queue.o : fs_loop_queue.c fs_loop_queue.h
	$(MAKE_CMD) -c $< $(INCLUDE) $(LDFLAG)

install:
	cp $(TARGET_NAME) /usr/local/lib

clean:
	rm -rf $(OBJECTS)
