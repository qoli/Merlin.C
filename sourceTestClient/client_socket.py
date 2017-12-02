# Client using basic Python socket to connect to google.com and request mainpage
#
# Client is a system that connects to a remote system (the Server) to fetch data
#

import socket
import sys

try:
    #AF_NET = Address family IPv4
    #SOCK_STREAM = connection oriented TCP protocol
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
except socket.error, msg:
    print 'Error making the socket'

print 'Socket Created'

host = 'www.google.com'
#port 80 reserved for HTTP (WWW)
port = 80

try:
    remote_ip = socket.gethostbyname(host)
except socket.gaierror:
    print 'Could not resove hostname'
    sys.exit()

print 'IP address of host is ' + remote_ip

#Connect to remote server
s.connect((remote_ip, port))

print 'Socket connected to ' + host + ' on IP: ' + remote_ip

message = "GET / HTTP/1.1\r\n\r\n"

try:
    #send the whole string, which is actually an HTTP command to fetch the mainpage
    s.sendall(message)
    
except socket.error:
    print 'Send failed'
    sys.exit()

print 'Message send successful'

#receive the data
reply = s.recv(4096)

print reply
s.close()