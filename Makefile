libscheduler.so:so_scheduler.o
	gcc -pthread -shared so_scheduler.o -o libscheduler.so -g

so_scheduler.o:so_scheduler.c so_scheduler.h
	gcc -fPIC -c so_scheduler.c -g

 
clean:
	rm *.o libscheduler.so