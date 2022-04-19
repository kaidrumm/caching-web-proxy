README

Usage: ./webproxy <port> <cache timeout>

make clean -> remove the cache and the logs
make -> compile the proxy

make clean does not clear the blacklist and dns file. You can modify them manually, but please do not delete them.

DNS file format: 
hostname,ip
hostname,ip

Recommended sites for testing:
http://www.httpvshttps.com/
http://web.cs.wpi.edu/~rek/Grad_Nets/Spring2013/Program0_S13.pdf

Link prefetching not implemented.

Each thread logs its activity to a logfile.
Cache contains entire HTTP response messages - not just files. Headers are stored & copied veratim.

If the client stops accepting a file (usually due to timeout) during download from the server, proxy will continue downloading it so that the cached file is complete.

Most send and receive calls are blocking, except while the thread is idle during keepalive.

Despite blocking calls, sending files to client is done with select() to avoid overwhelming system buffers.

Cache timestamps and file mutexes are stored in a small global hashtable with linked-list entries at collisions.
Threads should not lock the hashtable and any individual file at the same time.

Proxy depends on the web browser to provide a HTTP request with valid headers.