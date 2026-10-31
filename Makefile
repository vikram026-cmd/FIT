# QNX SDP 8.0 / Raspberry Pi 5 (aarch64le)
PLATFORM ?= aarch64le
QCC ?= qcc
QNX_VARIANT ?= -Vgcc_nto$(PLATFORM)

TARGET := patient_monitor
SOURCE := src/patient_monitor.c
SECRETS := pass/secrets.h

CPPFLAGS := -Isupport -D_QNX_SOURCE -D_POSIX_C_SOURCE=200809L
CFLAGS := $(QNX_VARIANT) -std=c11 -Wall -Wextra -Wpedantic -O2 -g
LDLIBS := -pthread

# Override these when the Pi's address changes.
PI_USER ?= qnxuser
PI_HOST ?= 169.254.161.12
PI_DEST ?= /data/home/qnxuser/apps
SSH_KEY ?= $(USERPROFILE)/.ssh/qnx_pi
SSH_OPTS := -i "$(SSH_KEY)" -o IdentitiesOnly=yes -o BatchMode=yes -o MACs=hmac-sha2-256 -o Ciphers=aes128-ctr -o ConnectTimeout=10
SSH := ssh $(SSH_OPTS)

.PHONY: all build deploy run clean check-secrets

all: deploy

check-secrets:
	@test -f "$(SECRETS)" || (echo "ERROR: Create pass/secrets.h from pass/secrets.example.h and add your credentials."; exit 1)

build: check-secrets $(TARGET)

$(TARGET): $(SOURCE) support/pm_config.h $(SECRETS)
	$(QCC) $(CPPFLAGS) $(CFLAGS) -o "$@" "$(SOURCE)" $(LDLIBS)

deploy: build
	$(SSH) $(PI_USER)@$(PI_HOST) "mkdir -p $(PI_DEST)"
	$(SSH) $(PI_USER)@$(PI_HOST) "cat > $(PI_DEST)/$(TARGET)" < "$(TARGET)"
	$(SSH) $(PI_USER)@$(PI_HOST) "chmod 755 $(PI_DEST)/$(TARGET) && ls -l $(PI_DEST)/$(TARGET)"

run:
	$(SSH) $(PI_USER)@$(PI_HOST) "$(PI_DEST)/$(TARGET)"

clean:
	rm -f "$(TARGET)"
