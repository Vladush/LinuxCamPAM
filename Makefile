
BUILD_DIR = build
CPU_CORES = $(shell nproc)

.PHONY: all clean install uninstall deb deps models test help

help:
	@echo "LinuxCamPAM Makefile"
	@echo "--------------------"
	@echo "make all       : Build the project"
	@echo "make install   : Install to system (requires sudo)"
	@echo "make uninstall : Remove from system (requires sudo)"
	@echo "make deb       : Generate Debian package (.deb)"
	@echo "make test      : Run authentication test"
	@echo "make clean     : Remove build artifacts"
	@echo "make deps      : Install system dependencies"
	@echo "make models    : Download required ONNX models"

all:
	@mkdir -p $(BUILD_DIR)
	@cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	@cmake --build $(BUILD_DIR) -j$(CPU_CORES)

models:
	@./scripts/download_models.sh

deps:
	@./scripts/install_deps.sh

install: all
	@echo "Installing to system..."
	@sudo cmake --install $(BUILD_DIR)
	@sudo systemctl daemon-reload
	@sudo systemctl enable linuxcampam
	@sudo pam-auth-update --enable linuxcampam
	@echo "Installation complete. Run 'sudo linuxcampam add <username>' to enroll."

uninstall:
	@echo "Removing LinuxCamPAM from system..."
	@sudo systemctl stop linuxcampam 2>/dev/null || true
	@sudo systemctl disable linuxcampam 2>/dev/null || true
	@sudo pam-auth-update --remove linuxcampam 2>/dev/null || true
	@sudo rm -f /usr/bin/linuxcampam /usr/bin/linuxcampamd /usr/bin/check_opencl
	@sudo rm -f /lib/x86_64-linux-gnu/security/pam_linuxcampam.so
	@sudo rm -f /lib/systemd/system/linuxcampam.service
	@sudo rm -f /usr/share/pam-configs/linuxcampam
	@sudo rm -rf /etc/linuxcampam
	@sudo systemctl daemon-reload
	@echo "Uninstall complete."

deb:
	@echo "Building Debian package..."
	@dpkg-buildpackage -b -uc -us
	@echo "Package generated in parent directory:"
	@ls -lh ../linuxcampam_*.deb

test:
	@echo "Running authentication test..."
	@linuxcampam test

clean:
	@rm -rf $(BUILD_DIR)
	@rm -rf obj-x86_64-linux-gnu debian/.debhelper debian/linuxcampam debian/files debian/*.substvars debian/*.debhelper.log
	@echo "Cleaned."

