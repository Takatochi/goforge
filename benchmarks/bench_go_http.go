package main

import (
	"context"
	"fmt"
	"io"
	"math"
	"net"
	"net/http"
	"runtime"
	"sort"
	"sync"
	"sync/atomic"
	"time"
)

func pct(sorted []int64, q float64) int64 {
	if len(sorted) == 0 {
		return 0
	}
	idx := int(math.Ceil(q*float64(len(sorted)))) - 1
	if idx < 0 {
		idx = 0
	}
	if idx >= len(sorted) {
		idx = len(sorted) - 1
	}
	return sorted[idx]
}

func main() {
	mux := http.NewServeMux()
	mux.HandleFunc("/api/v1/users/42", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json; charset=utf-8")
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte(`{"id":42,"status":"ok"}`))
	})
	mux.HandleFunc("/api/v1/io-wait", func(w http.ResponseWriter, r *http.Request) {
		time.Sleep(10 * time.Millisecond)
		w.Header().Set("Content-Type", "application/json; charset=utf-8")
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte(`{"waited_ms":10}`))
	})

	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		panic(err)
	}
	srv := &http.Server{Handler: mux}
	go func() { _ = srv.Serve(ln) }()
	defer func() { _ = srv.Shutdown(context.Background()) }()

	addr := ln.Addr().String()
	fmt.Println("====================================================================================================")
	fmt.Printf("  Native Go (%s, GOMAXPROCS=%d) net/http Benchmark Suite\n", runtime.Version(), runtime.GOMAXPROCS(0))
	fmt.Println("====================================================================================================")

	// Workload 1: 32 Keep-Alive clients x 500 requests = 16,000 requests
	{
		const clients = 32
		const reqsPerClient = 500
		const total = clients * reqsPerClient

		perClientLat := make([][]int64, clients)
		var okReqs int64
		var wg sync.WaitGroup

		t0 := time.Now()
		for c := 0; c < clients; c++ {
			perClientLat[c] = make([]int64, 0, reqsPerClient)
			wg.Add(1)
			go func(idx int) {
				defer wg.Done()
				client := &http.Client{
					Transport: &http.Transport{
						MaxIdleConnsPerHost: 2,
						DisableCompression:  true,
					},
				}
				url := "http://" + addr + "/api/v1/users/42"
				for r := 0; r < reqsPerClient; r++ {
					r0 := time.Now()
					resp, err := client.Get(url)
					if err != nil {
						return
					}
					_, _ = io.Copy(io.Discard, resp.Body)
					_ = resp.Body.Close()
					perClientLat[idx] = append(perClientLat[idx], time.Since(r0).Microseconds())
					atomic.AddInt64(&okReqs, 1)
				}
			}(c)
		}
		wg.Wait()
		sec := time.Since(t0).Seconds()

		allLat := make([]int64, 0, total)
		for _, slice := range perClientLat {
			allLat = append(allLat, slice...)
		}
		sort.Slice(allLat, func(i, j int) bool { return allLat[i] < allLat[j] })
		rps := float64(okReqs) / sec
		fmt.Printf("1. Keep-Alive JSON API (32x500 = 16,000 reqs)  | %8.1f req/s | p50: %4d us | p99: %5d us | ok: %d/%d\n",
			rps, pct(allLat, 0.50), pct(allLat, 0.99), okReqs, total)
	}

	// Workload 2: 300 concurrent clients hitting 10ms handler
	{
		const clients = 300
		var okReqs int64
		var wg sync.WaitGroup
		client := &http.Client{
			Transport: &http.Transport{
				MaxIdleConnsPerHost: 400,
				MaxConnsPerHost:     400,
				DisableKeepAlives:   true,
				DisableCompression:  true,
			},
		}
		url := "http://" + addr + "/api/v1/io-wait"

		t0 := time.Now()
		for c := 0; c < clients; c++ {
			wg.Add(1)
			go func() {
				defer wg.Done()
				resp, err := client.Get(url)
				if err != nil {
					return
				}
				_, _ = io.Copy(io.Discard, resp.Body)
				_ = resp.Body.Close()
				if resp.StatusCode == 200 {
					atomic.AddInt64(&okReqs, 1)
				}
			}()
		}
		wg.Wait()
		ms := float64(time.Since(t0).Microseconds()) / 1000.0
		rps := float64(okReqs) / (ms / 1000.0)
		fmt.Printf("2. 300 Concurrent I/O Handlers (10ms sleep)    | %8.1f req/s | wall: %6.1f ms | goroutines M:N | ok: %d/%d\n",
			rps, ms, okReqs, clients)
	}
	fmt.Println("====================================================================================================")
}
