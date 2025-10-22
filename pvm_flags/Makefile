VIMTO=$(shell which vimto 2>/dev/null)

test-host:
	go test -v ./...

test-vimto:
	sudo $(VIMTO) -kernel ghcr.io/alban/ci-kernels:6.18-rc2-patch-alban-7ed61704d40f05ced85923829a849016db2fe3c2 -- go test -v .

