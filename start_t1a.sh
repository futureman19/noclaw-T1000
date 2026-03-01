#!/bin/bash
cd /home/ubuntu/.openclaw/workspace/projects/noclaw
export NOCLAW_TELEGRAM_TOKEN="8608842627:AAE1utGnW2ZoD6PsOypkLjX3nD1jRJCVpMI"
while true; do
    ./noclaw agent --channel telegram >> /tmp/t1a_telegram.log 2>&1
    sleep 5
done
