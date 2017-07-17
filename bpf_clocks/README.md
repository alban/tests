## bpf clocks

In 3 different shells:

```
sudo touch /foo_100_

sudo ./timestamp.py |grep -E 'mystat-.*(/foo_100_|kretprobe)'
sudo ./rettimestamp.py | grep -E 'mystat-.*(/foo_100_|kretprobe)'
./mystat |grep /foo_100_
```

Results: it is ordered correctly
```
98726105275230 userspace clock 1	diff:
98726105276644 kprobe			 1414ns
98726105294073 kretprobe		17429ns
98726105299573 userspace clock 2	 5500ns
```

On GCE:
```
7741725436455 userspace clock 1		diff:
7741725436685 kprobe			 230ns
7741725442317 kretprobe			5632ns
7741725442887 userspace clock 2		 570ns
```
