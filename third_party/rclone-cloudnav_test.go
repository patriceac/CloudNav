package oauthutil

import (
	"net"
	"net/http"
	"net/url"
	"testing"
)

func TestCloudNavReservedPortFallback(t *testing.T) {
	// Either occupy the normal port or let an OS reservation make it unavailable.
	busy, err := net.Listen("tcp", bindAddress)
	if err == nil {
		defer busy.Close()
	}
	providers := []struct {
		name         string
		redirectURL  string
		redirectHost string
	}{
		{name: "drive", redirectURL: RedirectURL, redirectHost: "127.0.0.1"},
		{name: "onedrive", redirectURL: RedirectLocalhostURL, redirectHost: "localhost"},
	}
	for _, provider := range providers {
		config := &Config{RedirectURL: provider.redirectURL}
		server, err := cloudNavAuthServer(provider.name, &Options{}, config)
		if err != nil {
			t.Fatalf("%s fallback failed: %v", provider.name, err)
		}
		u, parseErr := url.Parse(config.RedirectURL)
		_, listenerPort, listenerErr := net.SplitHostPort(server.listener.Addr().String())
		if parseErr != nil || listenerErr != nil || u.Hostname() != provider.redirectHost || u.Port() == bindPort || u.Port() != listenerPort {
			server.Stop()
			t.Fatalf("%s callback URL does not match the ephemeral listener: %q", provider.name, config.RedirectURL)
		}
		if config.MakeOauth2Config().RedirectURL != config.RedirectURL {
			server.Stop()
			t.Fatalf("%s token exchange does not retain callback URL", provider.name)
		}
		server.Stop()
	}

	// The local authorization endpoint must use the same callback URI and retain
	// OAuth state validation after selecting the fallback port.
	config := &Config{RedirectURL: RedirectURL, AuthURL: "https://accounts.google.com/o/oauth2/auth"}
	server, err := cloudNavAuthServer("drive", &Options{}, config)
	if err != nil {
		t.Fatal(err)
	}
	defer server.Stop()
	u, err := url.Parse(config.RedirectURL)
	if err != nil {
		t.Fatal(err)
	}
	server.state = "synthetic-state"
	server.authURL = config.MakeOauth2Config().AuthCodeURL(server.state)
	go server.Serve()
	client := &http.Client{CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
	for _, valid := range []bool{false, true} {
		state := "wrong-state"
		if valid {
			state = server.state
		}
		response, err := client.Get("http://" + u.Host + "/auth?state=" + state)
		if err != nil {
			t.Fatal(err)
		}
		response.Body.Close()
		if !valid && response.StatusCode != http.StatusForbidden {
			t.Fatal("invalid OAuth state was accepted")
		}
		if valid {
			location, err := response.Location()
			if err != nil || response.StatusCode != http.StatusTemporaryRedirect || location.Query().Get("redirect_uri") != config.RedirectURL {
				t.Fatal("authorization did not use the ephemeral callback URI")
			}
		}
	}

	other := &Config{RedirectURL: RedirectURL}
	if unexpected, err := cloudNavAuthServer("dropbox", &Options{}, other); err == nil {
		unexpected.Stop()
		t.Fatal("unrelated providers must not change their registered callback")
	}
	if other.RedirectURL != RedirectURL {
		t.Fatal("unrelated provider configuration changed")
	}
	custom := &Config{RedirectURL: "https://example.invalid/callback"}
	if unexpected, err := cloudNavAuthServer("drive", &Options{}, custom); err == nil {
		unexpected.Stop()
		t.Fatal("custom callback must not be replaced")
	}
}
