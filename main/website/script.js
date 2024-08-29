"use strict";

const network_status_data = {
    status: null,
    status_message: 'loading...',

    // Start continuously fetching `status` in the background.
    async fetch_update() {
        try {
            this.status = await (await fetch('/api/status', { signal: AbortSignal.timeout(5000) })).json();
            this.status_message = '';
        } catch (error) {
            this.status_message = `ERROR: Couldn't fetch status from server: ${error}`;
        }
        setTimeout(() => this.fetch_update(), 2000);
    }
};

const network_settings_data = {
    conf: {
        eth_use_dhcp: false,
        eth_ip: "loading...",
        eth_netmask: "loading...",
        eth_gw: "loading...",
        wifi_enabled: false,
        wifi_ssid: "loading...",
        wifi_pass: "",
        wifi_use_dhcp: false,
        wifi_ip: "loading...",
        wifi_netmask: "loading...",
        wifi_gw: "loading...",
        can_bitrate: 0
    },
    original_conf: null,
    status_message: "loading...",

    // Fetch the server's configuration.
    async fetch_update() {
        try {
            const text = await (await fetch('/api/config', { signal: AbortSignal.timeout(5000) })).text();
            this.original_conf = JSON.parse(text);
            this.conf = JSON.parse(text);
            this.status_message = '';
        } catch (error) {
            this.status_message = `ERROR: Couldn't fetch settings from server: ${error}`;
        }
    },

    // Submit the current configuration to the server.
    async submit() {
        if (this.original_conf === null) {
            this.status_message = "ERROR: No settings fetched from server. Try reloading."
        }

        // Trim string input fields
        for (const key of Object.keys(this.conf)) {
            if (typeof this.conf[key] === 'string') {
                this.conf[key] = this.conf[key].trim();
            }
        }

        // Make an object that only contains changed settings.
        const post_obj = {};
        for (const key of Object.keys(this.conf)) {

            if (this.conf[key] !== '' && this.conf[key] !== null && this.conf[key] !== this.original_conf[key]) {
                post_obj[key] = this.conf[key];
            }
        }

        // Return if there are no fields to post.
        if (Object.keys(post_obj).length === 0) {
            this.status_message = 'Settings were not saved because no fields were modified.';
            return;
        }

        // Post the settings
        try {
            const response = await fetch('/api/config', {
                method: 'POST',
                body: new URLSearchParams(post_obj),
                signal: AbortSignal.timeout(5000)
            });
            this.status_message = await response.text();

            // If the response is OK, the server will restart,
            // so reload the page
            if (response.ok) {
                setTimeout(() => {
                    window.location.reload();
                }, 2000);
            }
        } catch (error) {
            this.status_message = `ERROR: Couldn't post settings to server: ${error}`;
        }

    }
};