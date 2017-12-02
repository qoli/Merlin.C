import socket, sys, select, string

def prompt():
    sys.stdout.write('<You> ')
    sys.stdout.flush()
    
if __name__ == "__main__":
    
    if(len(sys.argv) < 3):
        print 'Usage : python telnet.py hostname port'
        sys.exit()
        
    host = sys.argv[1]
    port = int(sys.argv[2])
    
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(2)
    
    try:
        s.connect((host, port))
    except:
        print 'Connection error'
        sys.exit()
        
    print 'Connected to host'
    prompt()

    while True:
        socket_list = [sys.stdin, s]

        read_sockets, write_sockets, error_sockets = select.select(socket_list, [], [])
        
        for sock in read_sockets:
            #incoming messages
            if sock == s:
                data = sock.recv(4096)
                if not data:
                    print '\nConnection error'
                    sys.exit()
                else:
                    sys.stdout.write(data)
                    prompt()
                    
            else:
                msg = sys.stdin.readline()
                s.send(msg)
                prompt()