package gitp

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"
)

type transport struct {
	client  *http.Client
	baseURL string
}

func newTransport(baseURL string) *transport {
	return &transport{
		client:  &http.Client{Timeout: 2 * time.Minute},
		baseURL: baseURL,
	}
}

func (t *transport) GetRefs(ctx context.Context) ([]byte, error) {
	return t.do(ctx, http.MethodGet, buildURL(t.baseURL, "info/refs?service=git-upload-pack"), nil, map[string]string{
		"Git-Protocol": "version=1",
	}, nil)
}

func (t *transport) UploadPack(ctx context.Context, reqBody []byte, progress Progress) ([]byte, error) {
	return t.do(ctx, http.MethodPost, buildURL(t.baseURL, "git-upload-pack"), reqBody, map[string]string{
		"Git-Protocol": "version=1",
		"Content-Type": "application/x-git-upload-pack-request",
		"Accept":       "application/x-git-upload-pack-result",
	}, progress)
}

func (t *transport) do(ctx context.Context, method, url string, body []byte, headers map[string]string, progress Progress) ([]byte, error) {
	var reader io.Reader
	if body != nil {
		reader = bytes.NewReader(body)
	}
	req, err := http.NewRequestWithContext(ctx, method, url, reader)
	if err != nil {
		return nil, err
	}
	req.Header.Set("User-Agent", "git/2.54.0")
	for key, value := range headers {
		req.Header.Set(key, value)
	}
	resp, err := t.client.Do(req)
	if err != nil {
		return nil, fmt.Errorf("HTTP request failed: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode >= 400 {
		return nil, fmt.Errorf("HTTP error %d", resp.StatusCode)
	}
	data, err := readAllWithProgress(resp.Body, progress)
	if err != nil {
		return nil, fmt.Errorf("failed to read HTTP response: %w", err)
	}
	return data, nil
}

func readAllWithProgress(r io.Reader, progress Progress) ([]byte, error) {
	if progress == nil {
		return io.ReadAll(r)
	}
	buf := bytes.Buffer{}
	chunk := make([]byte, 32*1024)
	var total int64
	for {
		n, err := r.Read(chunk)
		if n > 0 {
			buf.Write(chunk[:n])
			total += int64(n)
			progress.Download(total, false)
		}
		if err == io.EOF {
			progress.Download(total, true)
			return buf.Bytes(), nil
		}
		if err != nil {
			return nil, err
		}
	}
}

func buildURL(base, suffix string) string {
	base = strings.TrimRight(base, "/")
	suffix = strings.TrimLeft(suffix, "/")
	return base + "/" + suffix
}
