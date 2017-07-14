In a shell:
```
$ sudo ./timestamp.py |awk '{print $7 ";" $9}'|grep /foo_100_
95560362623493;/foo_100_
```

In another shell:
```
$ ./stat |grep /foo_100_
95560362622753 95560362630612 /foo_100_
```

It is ordered correctly:
```
95560362622753
95560362623493
95560362630612
```
