"use strict";

const network_status_data = {
    status: null,
    status_message: 'loading...',

    async initialize() {
        try {
            this.status = await (await fetch('/api/status')).json();
            this.status_message = '';
        } catch (error) {
            this.status_message = `ERROR: Couldn't fetch status from server: ${error}`;
        }
    },
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

    async initialize() {
        try {
            const text = await (await fetch('/api/config')).text();
            this.original_conf = JSON.parse(text);
            this.conf = JSON.parse(text);
            this.status_message = '';
        } catch (error) {
            this.status_message = `ERROR: Couldn't fetch settings from server: ${error}`;
        }
    },

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

        if (Object.keys(post_obj).length !== 0) {
            try {
                const response = await fetch('/api/config', {
                    method: 'POST',
                    body: new URLSearchParams(post_obj)
                });
                this.status_message = await response.text();
                if (response.ok) {
                    setTimeout(() => {
                        window.location.reload();
                    }, 2000);
                }
            } catch (error) {
                this.status_message = `ERROR: Couldn't post settings to server: ${error}`;
            }
        } else {
            this.status_message = 'No request sent because no fields were updated.';
        }
    },
};