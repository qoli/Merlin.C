# TCP Chat server which listens for incoming connections from chat clients
# uses port 8000
#
#

import socket, select, sys
from thread import *

def broadcast_data(sock, message):
    for socket in CONNECTION_LIST:
        #if the socket is not the server or the client from which the message originated
        if socket != server_socket and socket != sock:
            try:
                socket.send(message)
            except:
                #if the socket connection is broke, close the socket and remove it
                socket.close()
                CONNECTION_LIST.remove(socket)
                
if __name__ == "__main__":
    
    #keep list of all sockets
    CONNECTION_LIST = []
    RECV_BUFFER = 4096 #fairly arbitrary buffer size, specifies maximum data to be recieved at once
    PORT = 8000
    
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_socket.bind(("0.0.0.0", PORT))
    server_socket.listen(10)
    
    CONNECTION_LIST.append(server_socket)
    
    print "Chat server started on port " + str(PORT)
    
    while True:
        read_sockets, write_sockets, error_sockets = select.select(CONNECTION_LIST, [], [])
        
        for sock in read_sockets:
            #new connection
            if sock == server_socket:
                sockfd, addr = server_socket.accept()
                CONNECTION_LIST.append(sockfd)
                print "Client (%s, %s) connected" % addr
                
                broadcast_data(sockfd, "[%s:%s] entered room\n" % addr)
                
            #incoming message from client
            else:
                try:
                    data = sock.recv(RECV_BUFFER)
                    if data:
                        broadcast_data(sock, "\r" + '<' + str(sock.getpeername()) + '> ' + data)
                    
                except:
                    broadcast_data(sock, "Client (%s, %s) if offline" %addr)
                    sock.close()
                    CONNECTION_LIST.remove(sock)
                    countinue
    
    server_socket.close()
                