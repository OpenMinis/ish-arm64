package main

import (
	"fmt"
	"net"
	"os"
)

func main() {
	ifaces, e := net.Interfaces()
	if e != nil {
		panic(e)
	}
	usable := 0
	for _, i := range ifaces {
		a, e := i.Addrs()
		if e != nil {
			panic(e)
		}
		fmt.Printf("%d %s %s mtu=%d addr=%v\n", i.Index, i.Name, i.Flags, i.MTU, a)
		if i.Flags&net.FlagUp != 0 && i.Flags&net.FlagLoopback == 0 && len(a) > 0 {
			usable++
		}
	}
	fmt.Printf("interfaces=%d usable=%d\n", len(ifaces), usable)
	if usable == 0 {
		os.Exit(1)
	}
}
