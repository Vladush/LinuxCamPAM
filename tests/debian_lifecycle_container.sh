#!/bin/bash
set -euo pipefail

test -f /.dockerenv || { echo "Refusing to modify PAM outside a disposable Docker container." >&2; exit 2; }
apt-get update
apt-get install -y --no-install-recommends /package.deb
test "$(dpkg-query -W -f='${Status}' linuxcampam)" = 'install ok installed'
test "$(stat -c '%a' /etc/linuxcampam/users)" = 700
grep -q 'pam_linuxcampam.so' /etc/pam.d/common-auth

# Exercise an administrator edit together with a changed vendor default.
printf '\n; local lifecycle edit\n' >> /etc/linuxcampam/config.ini
cp /etc/linuxcampam/config.ini /tmp/expected.ini
printf '{}\n' > /etc/linuxcampam/users/lifecycle.json
chmod 600 /etc/linuxcampam/users/lifecycle.json
dpkg-deb -R /package.deb /tmp/upgrade
version=$(dpkg-query -W -f='${Version}' linuxcampam)
sed -i "s/^Version: .*/Version: $version+lifecycle1/" /tmp/upgrade/DEBIAN/control
printf '\n; changed vendor default\n' >> /tmp/upgrade/etc/linuxcampam/config.ini
dpkg-deb --build /tmp/upgrade /tmp/upgrade.deb
dpkg --force-confold --install /tmp/upgrade.deb
cmp /tmp/expected.ini /etc/linuxcampam/config.ini
test "$(dpkg-query -W -f='${Version}' linuxcampam)" = "$version+lifecycle1"
test "$(stat -c '%a' /etc/linuxcampam/users)" = 700
test "$(stat -c '%a' /etc/linuxcampam/users/lifecycle.json)" = 600
grep -q 'pam_linuxcampam.so' /etc/pam.d/common-auth
test "$(grep -c '^/etc/linuxcampam/config.ini$' /var/lib/dpkg/info/linuxcampam.conffiles)" -eq 1

dpkg --remove linuxcampam
cmp /tmp/expected.ini /etc/linuxcampam/config.ini
test ! -e /usr/bin/linuxcampam
! grep -q 'pam_linuxcampam.so' /etc/pam.d/common-auth
dpkg --purge linuxcampam
test ! -e /etc/linuxcampam/config.ini
! grep -q 'pam_linuxcampam.so' /etc/pam.d/common-auth
# User-created biometric records remain private after package removal.
test "$(stat -c '%a' /etc/linuxcampam/users)" = 700
test "$(stat -c '%a' /etc/linuxcampam/users/lifecycle.json)" = 600
echo "Install, edited-config upgrade, remove, and purge passed."
