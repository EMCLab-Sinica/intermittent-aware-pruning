PORT=8080
REMOTE_USER='warrenanson'
REMOTE_HOST='140.112.29.93'
ssh -L 8080:localhost:$PORT $REMOTE_USER'@'$REMOTE_HOST
