#!/bin/bash
# Paste this whole file into the "Cloud-init script" box under Show advanced options ->
# Management when creating the Oracle Cloud Always Free VM (Ubuntu image; see the
# README's step-by-step for the rest of instance creation). Runs once, as root, on
# first boot.
#
# What this DOES: installs Docker, and opens THIS VM's own firewall for the
# rendezvous port. What it does NOT do: open the Oracle Cloud Security List, which is
# a separate, VCN-level firewall in front of the VM entirely - nothing running inside
# the VM can configure it, so that step stays manual in the README.
set -euo pipefail

# Docker Engine + the compose plugin from Docker's own apt repo - Ubuntu's own
# docker.io package is old enough on both 22.04 and 24.04 to be missing the
# `docker compose` subcommand ../docker-compose.yml is written for.
apt-get update
apt-get install -y ca-certificates curl
install -m 0755 -d /etc/apt/keyrings
curl -fsSL https://download.docker.com/linux/ubuntu/gpg -o /etc/apt/keyrings/docker.asc
chmod a+r /etc/apt/keyrings/docker.asc
# shellcheck disable=SC1091
. /etc/os-release
echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.asc] https://download.docker.com/linux/ubuntu ${UBUNTU_CODENAME:-$VERSION_CODENAME} stable" \
	> /etc/apt/sources.list.d/docker.list
apt-get update
apt-get install -y docker-ce docker-ce-cli containerd.io docker-compose-plugin

# Oracle's Ubuntu image pre-configures iptables to allow only SSH by default, entirely
# separate from - and evaluated BEFORE - the cloud Security List: a Security List rule
# alone does nothing if this drops the packet first. Documented repeatedly on OCI's own
# forums as the reason "I opened the port in the console and it still doesn't work".
# iptables-persistent is what makes the rule survive a reboot; DEBIAN_FRONTEND skips
# its interactive "save current rules now?" prompt, which cloud-init has no console for.
DEBIAN_FRONTEND=noninteractive apt-get install -y iptables-persistent
iptables -I INPUT -p udp --dport 24701 -j ACCEPT
netfilter-persistent save

# Nothing here builds or starts the server itself - that needs the source, which this
# script has no repository URL to assume. SSH in, get tools/rendezvous/ onto the VM
# (git clone your checkout, or scp just that directory), then:
#   cd tools/rendezvous && docker compose up -d --build
