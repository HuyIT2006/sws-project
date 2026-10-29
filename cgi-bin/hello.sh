#!/bin/sh
printf 'Content-Type: text/html\r\n\r\n'
printf '<h1>Hello CGI</h1><p>%s</p>' "$QUERY_STRING"
