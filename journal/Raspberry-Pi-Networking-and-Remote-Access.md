# Raspberry Pi networking and remote access: diagnosis and field guide

**Conversation date:** 8–9 October 2026 (Korea time)  
**Device:** `resilient-pi`, Ubuntu 24.04.5 LTS  
**Goal:** SSH to the Pi from anywhere, then run local network and concurrency tests when the Pi is moved from the dorm to home.

## Outcome and current verification status

- The Pi's dorm Ethernet address is `[DORM_ADDR_REDACTED]/24` on `eth0`. SSH to it works from the dorm network but timed out from a cellular hotspot. A packet capture on the Pi saw **zero incoming SSH SYNs** during the cellular attempt. This points to a block somewhere before the Pi; the school network boundary is the leading candidate, but the exact filtering device was not identified.
- Tailscale was installed on the Pi and Mac. The Pi's tailnet address is `[TAILSCALE_ADDR_REDACTED]`. SSH to that address succeeded from cellular, and an initial reboot while Ethernet was connected succeeded. Tailscale's `tailscaled` service was enabled at boot.
- Wi-Fi was configured on `wlan0` with Netplan. The Pi successfully joined the nearby `Hojune` WPA2 network, obtained `172.20.10.4/28`, and fetched an HTTPS page through `wlan0`.
- A first reboot with Ethernet unplugged left Tailscale unreachable during the observed attempts. Boot logs showed Wi-Fi connected and obtained DHCP at **00:44:05**, while `tailscaled` started only after Ethernet was reattached at **00:46:14–00:46:17**. `systemd-analyze critical-chain` traced the delay through `systemd-networkd-wait-online.service` and `cloud-init.service`.
- A persistent override was installed so `systemd-networkd-wait-online` accepts **either** routable Ethernet **or** routable Wi-Fi with IPv4. With Ethernet unplugged, a manual invocation of that exact condition exited `0`; the route to `1.1.1.1` used `wlan0`, and Tailscale SSH worked. **The conversation ended before the result of the final Ethernet-unplugged reboot was reported.** Perform the final check below before relying on cable-free boot at home.
- The actual home router and home Wi-Fi credentials have **not** been tested in this conversation. A successful connection to the `Hojune` network validates the Pi's Wi-Fi hardware and setup, but not a different router or password.

## The original SSH failure, step by step

1. `route -n get [DORM_ADDR_REDACTED]` on the Mac first showed `utun5`, a tunnel interface. After VPN/Tailscale were disabled, it showed `en0` through hotspot gateway `172.20.10.1`. The failure persisted, so the tunnel was not sufficient to explain it.
2. From cellular, `nc -vz -G 5 [DORM_ADDR_REDACTED] 22` timed out. A timeout means no useful TCP response came back; by itself it does not say which network dropped the packet.
3. On the Pi, `tcpdump` filtered for new inbound TCP SYNs to port 22. It observed none during the Mac's cellular attempt. The packet did not reach the Pi's SSH listener.
4. `ip -br -4 addr` confirmed that `[DORM_ADDR_REDACTED]/24` belongs directly to the Pi's `eth0`; this is not merely an address forwarded from another local router.
5. The same `nc` test **succeeded** when the Mac joined the dorm network. Thus the Pi's SSH service works and the failure depends on the external path. An inbound school firewall or access rule is plausible, though these observations do not prove which device implements it.

**Key distinction:** A *static* IP is stable, and this one is assigned to the Pi. It does not imply that the network permits unsolicited inbound connections from the wider internet. The school can permit dorm traffic and outbound connections while filtering outside-to-inside SSH.

## How Tailscale solves the access problem

Tailscale is software for creating an authenticated **private overlay network**, called a *tailnet*, among devices in different physical networks. It is not itself a single protocol. It uses **WireGuard** to authenticate peers and encrypt traffic, plus coordination, access rules, NAT traversal, and relays. The Mac and Pi must both be authorized in the same tailnet.

For ordinary SSH over Tailscale, the path is conceptually:

`Mac SSH client → encrypted Tailscale transport → Pi tailscaled → tailscale0 → Linux TCP stack → sshd on port 22`

The Pi and Mac attempt a direct encrypted connection. If the networks prevent it, their outbound connections can reach a **DERP relay**, which forwards already encrypted WireGuard packets. The relay does **not** need the keys to decrypt the SSH traffic. SSH separately authenticates and encrypts the login session. A relay can add latency and reduce throughput compared with a direct path. If the school also blocks the outbound connectivity needed by Tailscale, Tailscale cannot make the connection.

Tailscale does not remove `[DORM_ADDR_REDACTED]` from `eth0`. It adds `tailscale0` and an address such as `[TAILSCALE_ADDR_REDACTED]`; the ordinary dorm address and tailnet address coexist. With Tailscale running, `ssh hojune@[TAILSCALE_ADDR_REDACTED]` uses the private tailnet path. `ssh hojune@[DORM_ADDR_REDACTED]` still uses the ordinary school network path and its inbound rules. Installing Tailscale also does not, by itself, prevent local or dorm access to the Pi.

**Private SSH versus a public service:** Only authorized tailnet members can use the Pi's tailnet address. If visitors without Tailscale later need a website or API, publish that service separately through an approved HTTPS endpoint/tunnel or suitable hosting. Do not equate making a web app public with exposing SSH port 22. Cloudflare Tunnel is one outbound-only option for publishing or controlling access to services; check dorm hosting policy first.

**Industry analogy:** Organizations often use corporate VPNs, identity-aware access systems, bastion hosts, or agent-based management such as AWS Systems Manager. The common aim is to authenticate employees and grant specific private access without giving every internal server an unrestricted public SSH port.

## Network concepts asked about

| Term | Meaning and this Pi's example |
|---|---|
| **Network interface** | The operating system's attachment point to a network. `eth0` is Ethernet, `wlan0` is Wi-Fi, `tailscale0` is software-created, and `lo` is local loopback. `ip -br -4 addr` lists concise IPv4 interface information. An interface can be `UP` without a usable connection; `NO-CARRIER` means it has not established a physical/Wi-Fi link. |
| **TCP socket** | An application's endpoint identified by protocol, local address, and port. `sshd` listens on TCP port 22. Packets arrive through an interface, and the kernel delivers matching packets to the socket. `0.0.0.0:22` means the daemon listens on all local IPv4 addresses, but does not guarantee packets can cross upstream firewalls. Check with `sudo ss -ltnp '( sport = :22 )'`. |
| **Service and daemon** | A service is a managed background function/program. `systemd` starts, stops, and monitors units such as `tailscaled.service`. The `d` in `sshd`, `tailscaled`, and `networkd` conventionally means *daemon*, a background process. `systemctl is-enabled tailscaled` checks startup configuration; `systemctl is-active tailscaled` checks its current process state. An active process does not prove internet or peer connectivity. |
| **Subnet and prefix** | A prefix such as `/24` or `/28` marks the network portion of an IP address. Two hosts in the same subnet can usually communicate locally if the network permits it. The Pi's temporary `172.20.10.4/28` belongs to `172.20.10.0/28`; `172.20.10.15` is that subnet's **broadcast address**, not a valid Pi host address. Same Wi-Fi name does not necessarily imply reachability: guest/client isolation may block peers. |
| **Route** | A kernel rule choosing an outgoing interface and next hop for a destination. `ip -4 route` lists routes, and `ip -4 route get 1.1.1.1` shows the selected route. After Ethernet was unplugged, the Pi selected `via 172.20.10.1 dev wlan0`. For devices on the same subnet, the local interface normally carries the traffic without a gateway as next hop. |
| **Gateway** | A router used for destinations outside the local subnet, such as the home router or the hotspot at `172.20.10.1`. A default route is the fallback for destinations with no more specific route. |
| **DHCP** | A protocol through which a router/server leases an IP address and usually provides subnet, gateway, and DNS settings. `dhcp4: true` requested IPv4 settings for `wlan0`. A DHCP Wi-Fi address can change; discover it again after reconnecting. |
| **DNS and nameserver** | DNS maps names to addresses. A nameserver answers DNS lookups; DHCP may supply its address. `ssh hojune@[TAILSCALE_ADDR_REDACTED]` already specifies a numeric destination, while `curl https://example.com` needs name resolution first. If numeric IP connectivity works but hostnames do not resolve, investigate DNS. |
| **NAT** | Network Address Translation rewrites address/port information, often letting many local devices use one external connection. A hotspot commonly uses NAT. It typically tracks outgoing connections so replies can return; a new, unsolicited incoming connection has no such established mapping unless configured. The Pi's dorm IP was observed directly on `eth0`; the exact dorm failure was **not proven** to be NAT. NAT and firewall filtering are distinct. |
| **Relay / DERP** | A relay forwards packets between endpoints when they cannot establish a direct path. DERP is Tailscale's encrypted-packet relay system. It handles encrypted traffic, not plaintext, and can be slower than a direct connection. |
| **WireGuard** | The VPN protocol Tailscale uses for peer authentication and encrypted packet transport. Tailscale adds device coordination, access management, and connectivity mechanisms around it. |
| **Netplan and renderer** | Netplan reads YAML network declarations and generates configuration for a backend, called a *renderer*. This Ubuntu Server uses `systemd-networkd` as renderer. `netplan generate` checks and generates files; `netplan try --timeout 120` temporarily applies changes with a rollback prompt. |
| **wpa_supplicant** | The component that scans for and authenticates to password-protected Wi-Fi. `systemd-networkd` then configures IP networking, including DHCP, on the joined interface. |

## Pi configuration and observed tests

### Tailscale

The Pi runs Ubuntu 24.04 (Noble). Tailscale was installed from its signed Ubuntu package repository, the `tailscaled` service was enabled, and `sudo tailscale up` authorized the Pi in the same tailnet as the Mac. The Pi's Tailscale IP was obtained with `tailscale ip -4` and observed on `tailscale0` with `ip addr`.

From a cellular-connected Mac, `tailscale ping [TAILSCALE_ADDR_REDACTED]` and `ssh hojune@[TAILSCALE_ADDR_REDACTED]` were used to test access. `tailscale status` or `tailscale ping` can identify a direct versus relay path. The ordinary SSH server was retained; the optional Tailscale SSH feature was not required.

### Wi-Fi

At first `wlan0` showed `NO-CARRIER`, as the configured home network was not in range. `systemctl is-active netplan-wpa-wlan0.service` returned `active`, and `sudo wpa_cli -i wlan0 status` showed `SCANNING`. Repeated `Failed to initiate sched scan` messages did not establish that Wi-Fi was broken: `wpa_cli scan` returned `FAIL-BUSY` because scanning was already in progress, while `wpa_cli scan_results` listed nearby networks. Once the `Hojune` network was added, `wpa_state=COMPLETED`, `ssid=Hojune`, and `ip_address=172.20.10.4` confirmed association and DHCP. `curl -4 --interface wlan0 -I --connect-timeout 5 https://example.com` returned HTTP 200, proving outbound internet over Wi-Fi.

The Wi-Fi profile is in `/etc/netplan/60-home-wifi.yaml` and contains passwords, so **do not paste its contents**. Its file permission was `600` (`root` read/write only). The separate `/etc/netplan/70-eth-optional.yaml` was first created with permission `644`, causing Netplan's warning that it was accessible to other users. `sudo chmod 600 /etc/netplan/70-eth-optional.yaml` removed the warning. The `install -m 600 /dev/null PATH` command creates an empty file with that intended permission; an editor or later write may alter it, so verify with `stat -c '%a %U:%G %n' PATH`.

The `50-`, `60-`, and `70-` prefixes order Netplan files. `50-cloud-init.yaml` carries the original dorm Ethernet setup, `60-home-wifi.yaml` adds Wi-Fi, and `70-eth-optional.yaml` adds `optional: true` for Ethernet. The files are merged into a network definition. `optional: true` is intended to avoid waiting for an absent interface at boot; in this setup the running `networkctl status eth0` nevertheless reported `Required For Online: yes`, and the generated Ethernet `.network` file contained no `RequiredForOnline` entry. Therefore the final boot condition was set explicitly with a systemd override rather than inferred from the YAML alone.

**Known networks versus roaming:** With the present Netplan setup, the Pi automatically joins saved Wi-Fi networks; additional SSIDs/passwords can be added to `access-points`. For frequent interactive selection of unknown networks, NetworkManager and `nmcli --ask device wifi connect "SSID"` are more convenient, but changing network managers remotely can cut off access. Moving mainly between dorm Ethernet and known home Wi-Fi does not warrant that migration.

### Why the Mac could not SSH to the Pi's temporary Wi-Fi address

The Mac's `networksetup -getairportnetwork en0` said it was **not associated with an AirPort network**, and `ifconfig en0` showed `192.0.0.2/32` rather than an address in the Pi's `172.20.10.0/28` subnet. The Mac was not on the same local Wi-Fi as the Pi. Also, `172.20.10.15`, first tried as a destination, was not the Pi: it was the `/28` broadcast address. The Pi's actual leased address was `172.20.10.4`. `sshd` was confirmed listening on `0.0.0.0:22` and `[::]:22`; SSH via `[TAILSCALE_ADDR_REDACTED]` continued to work. This does **not** establish a fault with Tailscale or with the Pi's SSH listener. At home, first ensure the Mac and Pi actually join the same non-isolated Wi-Fi, discover the current Pi IP, and test it.

## The boot delay and targeted change

During the first Ethernet-free reboot, `systemd-networkd` associated `wlan0` and acquired its DHCP address at 00:44:05. At 00:46:14 the reattached Ethernet gained carrier; `systemd-networkd-wait-online` completed at 00:46:15 and `tailscaled` started at 00:46:17. The critical chain was:

`systemd-networkd → systemd-networkd-wait-online (about 1m 42s) → cloud-init → sysinit/basic → tailscaled`

After the configuration changes, the effective wait-online command was verified as:

```text
/usr/lib/systemd/systemd-networkd-wait-online --any --ipv4 --interface=eth0:routable --interface=wlan0:routable
```

The persistent override lives at `/etc/systemd/system/systemd-networkd-wait-online.service.d/90-either-link.conf`:

```ini
[Service]
ExecStart=
ExecStart=/usr/lib/systemd/systemd-networkd-wait-online --any --ipv4 --interface=eth0:routable --interface=wlan0:routable
```

After `sudo systemctl daemon-reload`, `systemctl show systemd-networkd-wait-online.service -p ExecStart --no-pager` confirmed the new command. With Ethernet unplugged and Wi-Fi online, a manual invocation with `--timeout=10` exited `0`, and `ip -4 route get 1.1.1.1` selected `wlan0`. The blank `ExecStart=` line clears the earlier systemd command before replacing it; `--any` means either named interface is sufficient, and `--ipv4` requires IPv4 connectivity. This is a **boot-readiness test**, not by itself proof that a public endpoint is reachable.

## Final verification and recovery

The final result was not included in the conversation. To close it out while still at the dorm:

1. Keep the known `Hojune` Wi-Fi available and **leave Ethernet unplugged**.
2. Run `sudo reboot` on the Pi. A broken SSH connection at reboot is expected.
3. After boot, from the Mac run `ssh hojune@[TAILSCALE_ADDR_REDACTED]`.
4. On the Pi run:

   ```bash
   uptime
   ip -br -4 addr show wlan0
   ip -4 route get 1.1.1.1
   systemctl is-active tailscaled netplan-wpa-wlan0.service
   systemd-analyze critical-chain tailscaled.service --no-pager
   ```

   Confirm that SSH connects without Ethernet, Wi-Fi has an IP, the internet route uses `wlan0`, both services are active, and the old wait-online delay is gone.
5. If the SSH attempt times out, reattach dorm Ethernet. Connect locally and inspect the current boot with `journalctl -b -u systemd-networkd-wait-online -u netplan-wpa-wlan0 -u tailscaled --no-pager` and `systemd-analyze critical-chain tailscaled.service --no-pager`. Do not assume that `tailscaled` being `active` means it can reach a peer.
6. At home, the separate home Wi-Fi profile must still be tested. If it joins, the home router gives `wlan0` a new address. With the Mac on the same home Wi-Fi, test `ssh hojune@<current-wlan0-IP>` for local tests; use `ssh hojune@[TAILSCALE_ADDR_REDACTED]` for tailnet access from elsewhere. A home guest network or client isolation can prevent direct peer access despite similar address ranges.

## Further reading

- [Tailscale: what it is](https://tailscale.com/docs/concepts/what-is-tailscale), [connection types](https://tailscale.com/docs/reference/connection-types), and [control versus data planes](https://tailscale.com/docs/concepts/control-data-planes).
- [Tailscale Linux installation](https://tailscale.com/docs/install/linux) and [SSH to a Raspberry Pi](https://tailscale.com/learn/how-to-ssh-into-a-raspberry-pi/).
- [Netplan examples](https://netplan.readthedocs.io/en/stable/examples/), [YAML options including `optional`](https://netplan.readthedocs.io/en/latest/netplan-yaml/), and [Netplan CLI commands](https://netplan.readthedocs.io/en/stable/cli/).
- [systemd-networkd-wait-online options](https://github.com/systemd/systemd/blob/main/man/systemd-networkd-wait-online.service.xml).
