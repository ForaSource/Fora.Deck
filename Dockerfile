FROM debian:13 AS linux-packaging

RUN sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/debian.sources \
    && apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates git build-essential cmake ninja-build python3 curl file dpkg-dev pkg-config \
    qt6-base-dev qt6-wayland qt6-tools-dev qt6-l10n-tools libx11-dev libxcb-randr0-dev libxcb-res0-dev libsecret-1-dev \
    xvfb xauth dbus-x11 desktop-file-utils \
    && test "$(qmake6 -query QT_VERSION)" = '6.8.2' \
    && test "$(getconf GNU_LIBC_VERSION)" = 'glibc 2.41' \
    && git config --global --add safe.directory /src

ENV LANG=C.UTF-8

WORKDIR /src
CMD ["sh", "-c", "cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/usr -DXLIBRARY_NATIVE_CPU=OFF && python3 /src/packaging/build_release.py --build-path /build --output-path /output/linux-x86_64 --cache-path /cache --format all"]

FROM linux-packaging AS startup-check

COPY . .

RUN cmake --preset linux-release -DCMAKE_PREFIX_PATH=/usr \
    && cmake --build --preset linux-release --parallel 2 \
    && cmake --install cmake-build-linux-release --prefix /opt/xlibrary \
    && desktop-file-validate \
       /opt/xlibrary/share/applications/io.github.LapplandJulia.XLibrary.desktop

ENV QT_QPA_PLATFORM=xcb

CMD ["xvfb-run", "-a", "dbus-run-session", "--", "/opt/xlibrary/bin/XLibrary", "--check-startup"]
