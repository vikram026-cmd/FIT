# =========================================================
# QNX SDP 8.0 - Raspberry Pi 5 Patient Monitor
# Single executable with five pthread threads
# Build + automatic SSH deployment
# =========================================================

# -------------------- BUILD CONFIGURATION --------------------

PLATFORM ?= aarch64le
QCC ?= qcc

TARGET := patient_monitor
SOURCE := src/patient_monitor.c
SECRETS := pass/secrets.h

QNX_VARIANT := -Vgcc_nto$(PLATFORM)

CPPFLAGS := -Isupport -D_QNX_SOURCE -D_POSIX_C_SOURCE=200809L

CFLAGS := $(QNX_VARIANT) \
          -std=c11 \
          -Wall \
          -Wextra \
          -Wpedantic \
          -O2 \
          -g

# QNX provides pthread APIs through its system libraries.
# No -pthread flag is required.
LDLIBS :=

# -------------------- RASPBERRY PI CONFIGURATION --------------

PI_USER ?= qnxuser
PI_HOST ?= 169.254.161.12
PI_DEST ?= /data/home/qnxuser/apps

SSH_KEY ?= $(USERPROFILE)/.ssh/qnx_pi

SSH_OPTS := -i "$(SSH_KEY)" \
            -o IdentitiesOnly=yes \
            -o BatchMode=yes \
            -o MACs=hmac-sha2-256 \
            -o Ciphers=aes128-ctr \
            -o ConnectTimeout=10

SSH := ssh $(SSH_OPTS)

# -------------------- TARGETS ---------------------------------

.PHONY: all build deploy run clean check-secrets rebuild

# Momentics executes make all.
# Build first, then deploy.
all: deploy

# -------------------- SECRET CHECK ----------------------------

check-secrets:
	@test -f "$(SECRETS)" || \
	(echo "ERROR: Missing pass/secrets.h"; \
	 echo "Create it using pass/secrets.example.h"; \
	 exit 1)

# -------------------- BUILD -----------------------------------

build: check-secrets $(TARGET)

$(TARGET): $(SOURCE) support/pm_config.h $(SECRETS)
	$(QCC) $(CPPFLAGS) $(CFLAGS) -o "$@" "$(SOURCE)" $(LDLIBS)

# -------------------- DEPLOY ----------------------------------

deploy: build
	@echo "=========================================="
	@echo "Deploying Patient Monitor to Raspberry Pi"
	@echo "=========================================="

	$(SSH) $(PI_USER)@$(PI_HOST) \
		"mkdir -p '$(PI_DEST)'"

	$(SSH) $(PI_USER)@$(PI_HOST) \
		"rm -f '$(PI_DEST)/$(TARGET)'"

	$(SSH) $(PI_USER)@$(PI_HOST) \
		"cat > '$(PI_DEST)/$(TARGET)'" < "$(TARGET)"

	$(SSH) $(PI_USER)@$(PI_HOST) \
		"chmod 755 '$(PI_DEST)/$(TARGET)' && \
		 ls -l '$(PI_DEST)/$(TARGET)'"

	@echo "=========================================="
	@echo "Deployment completed"
	@echo "=========================================="

# -------------------- RUN -------------------------------------

run:
	$(SSH) $(PI_USER)@$(PI_HOST) \
		"'$(PI_DEST)/$(TARGET)'"

# -------------------- CLEAN -----------------------------------

clean:
	rm -f "$(TARGET)"

# -------------------- REBUILD ---------------------------------

rebuild: clean all