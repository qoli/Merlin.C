# Server in Python using sockets library
# Server uses sockets to recieve incoming connections and provide with data, opposite of the Client
# Server --> Open socket, bind to an addr, listen for incoming connections, accept connections, read/send
#

import socket
import sys
from thread import *

HOST = '' #Simply implies allow any host connection
PORT = 8000 #Arbitrary 4 digit number, avoiding using the smaller ports which are designated for system use

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
print 'Socket created'

#Bind the specified port to the socket, capture all incoming data
try:
    s.bind((HOST, PORT))
except socket.error, msg:
    print 'Bind failed. Error: ' + str(msg[0]) + ' Message ' + msg[1]
    sys.exit()
    
print 'Socket bind complete'

#listen for any incoming connections over this port, 10 means only 10 connections are allowed to be waiting
s.listen(10)
print 'Socket now listening'

def client_thread(conn):
    """
    Establishes connection the the client and creates threads
    """
    conn.send("Welcome to the server. Type a message and press enter: ")
    
    while True:
        #recieve client data
        data = conn.recv(1024)
        reply = 'OK...' + data
        if not data:
            break
            
        conn.sendall(reply)
        
    #Close connection after leaving the loop    
    conn.close()
    
while True:
    #wait to accept a conenction
    conn, addr = s.accept()
    #display client information
    print 'Connected with ' + addr[0] + ':' + str(addr[1])

    #start a new thread
    #Now send information to the client
    start_new_thread(client_thread, (conn,))

s.close()