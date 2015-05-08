## Telejson (formaly called tg-for-pytg2)

# Changed Project URL: [https://github.com/luckydonald/telejson](https://github.com/luckydonald/telejson)

To update your local clone, type 
``` sh
git remote set-url origin https://github.com/luckydonald/telejson.git
```

Modified version of Telegram-CLI, which sends you the events as json.    

You provide the ip/port you are listening for new messages with the -s switch:    
For example: ```telegram-cli -s 127.0.0.1:4458```
Like the original you can specify a port to listen for commands with the p flag, e.g. ```-p 1337``` and do inputs, like sending a message ```msg luckydonald "test message"```.

If you use pytg, you don't have to worry about the socket communication stuff below.

# Json Documentation #

The json response is documented in the ```results.txt``` text file.

# Communication looks like this: #

'you' is your Program, waiting for messages.
'cli' is this executable.

New is: You get a Length first, and you have to ```ACK```nowledge Messages.

1.  You ```listen``` on the specified port (e.g. ```4458```) for incomming connection
2.  When the telegram cli got a message, it will try to connect to that port.
3.  You have to ```accept``` that connection.
4.  Now the cli sends you the lenght of the json output:
    ```LENGTH```, a whitespace, int with 8 characters (filled with 0s in front), terminated by a newline ```\n```.
    The maximal possible length is ```33554432``` (```(1 << 25)```), minimal is theoretically ```00000000```, but that should not happen.
5.  You ```read``` (or ```recv```) 16 characters: 6+space+8+newline = 16 characters, or until the newline character.
6.  If you could parse that, you reply (```write```/```send```) "```ACK```" (acknowledged) or "```ERR```" (error).
7.  The CLI waits 10 seconds for an ```ACK``` reply. If it fails, it will close the socket, and send it again, restarting at step 1. If it receives ```ERR``` it will retry too. Continues with ```ACK```.    
    If you should do this non-blocking, and handle timeout (~10 seconds) error, in case the cli was not waiting for a response.
8.  The CLI sends you the json. Yay!
9.  You read LENGTH characters.
10. You send either ```ACK``` or ```ERR``` again
11. CLI checks again for ```ACK```, like in step 7, and if failed, closes the connection and restart at step 1.
12. The CLI ```close```s the connection, you too.



Also, I am thinking about renaming this to "telejson" as well as switching hosting to github.com, so the URL might change too.