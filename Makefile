
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


# Docker Build Targets
docker-amd64:
	@echo "Building Docker (amd64)..."
	@docker build --platform linux/amd64 -t linuxcampam:amd64 -f docker/Dockerfile.amd64 .
	@echo "Verifying..."
	@docker run --rm --platform linux/amd64 --entrypoint /usr/bin/linuxcampam linuxcampam:amd64 help

docker-arm64:
	@echo "Building Docker (arm64)..."
	@docker build --platform linux/arm64 -t linuxcampam:aarch64 -f docker/Dockerfile.aarch64 .
	@echo "Verifying..."
	@docker run --rm --platform linux/arm64 --entrypoint /usr/bin/linuxcampam linuxcampam:aarch64 help

docker-riscv64:
	@echo "Building Docker (riscv64) - This may take a long time..."
	@docker build --platform linux/riscv64 -t linuxcampam:riscv64 -f docker/Dockerfile.riscv64 .
	@echo "Verifying..."
	@docker run --rm --platform linux/riscv64 --entrypoint /usr/bin/linuxcampam linuxcampam:riscv64 help

docker-i386:
	@echo "Building Docker (i386)..."
	@docker build --platform linux/386 -t linuxcampam:i386 -f docker/Dockerfile.i386 .
	@echo "Verifying..."
	@docker run --rm --platform linux/386 --entrypoint /usr/bin/linuxcampam linuxcampam:i386 help

test:
	@echo "Running authentication test..."
	@linuxcampam test

clean:
	@rm -rf $(BUILD_DIR)
	@rm -rf obj-x86_64-linux-gnu debian/.debhelper debian/linuxcampam debian/files debian/*.substvars debian/*.debhelper.log
	@echo "Cleaned."

