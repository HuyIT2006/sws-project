# sws

`sws` là HTTP/1.0-compatible web server viết bằng C cho đồ án CS631.
Server hỗ trợ GET, HEAD, HTTP/1.1 downgrade, document root, directory indexing,
`/~user/sws`, CGI (`-c`), request logging (`-l`), IPv4/IPv6 và daemon mode.

## Build và chạy

```sh
make clean
make
./sws -d -p 8081 www
```

Mở `http://127.0.0.1:8081/` trong trình duyệt.

Chạy ngầm:

```sh
./sws -p 8081 www
```

Tùy chọn:

- `-c dir`: thư mục CGI
- `-d`: debug mode, không daemonize, log ra stdout
- `-i address`: bind IPv4 hoặc IPv6
- `-l file`: ghi request log
- `-p port`: listening port, mặc định 8080

## Demo

```sh
curl -i http://127.0.0.1:8081/
curl -I http://127.0.0.1:8081/index.html
curl -i http://127.0.0.1:8081/not-found
curl -i -X POST http://127.0.0.1:8081/index.html
curl -i http://127.0.0.1:8081/demo/
```

Demo CGI:

```sh
./sws -d -p 8081 -c cgi-bin www
curl -i 'http://127.0.0.1:8081/cgi-bin/hello.sh?name=CS631'
```

Demo logging:

```sh
./sws -d -p 8081 -l access.log www
cat access.log
```

Trong daemon mode, server tạo process riêng cho từng connection.
