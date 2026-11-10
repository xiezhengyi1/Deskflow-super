#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
xcode-select -p >/dev/null
source_root="$PWD"
export HOMEBREW_NO_AUTO_UPDATE=1
brew install cmake qtbase qttools qttranslations qtsvg openssl@3
qt_base="$(brew --prefix qtbase)"
qt_tools="$(brew --prefix qttools)"
qt_svg="$(brew --prefix qtsvg)"
qt_translations="$(brew --prefix qttranslations)/share/qt/translations"
qt_prefixes="$qt_base;$qt_tools;$qt_svg"
export PATH="$(brew --prefix)/bin:$qt_tools/bin:$qt_base/bin:$PATH"
build_dir="$source_root/build-macos-lbm"
cmake -S "$source_root" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=Release \
  "-DCMAKE_PREFIX_PATH=$qt_prefixes" \
  "-DQT_ADDITIONAL_PACKAGES_PREFIX_PATH=$qt_prefixes" \
  "-DQT_ADDITIONAL_HOST_PACKAGES_PREFIX_PATH=$qt_tools" \
  "-D_QT_QM_FILE=$qt_translations/qtbase_en.qm" \
  "-DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)" \
  -DBUILD_OSX_BUNDLE=ON -DBUILD_INSTALLER=OFF -DBUILD_TESTS=ON -DSKIP_BUILD_TESTS=ON
cmake --build "$build_dir" --parallel 2 --target \
  Deskflow deskflow-core LocalCursorTests FileRelayTests FileTransferTests ServerProxyTests ClientProxyTests ServerTests SettingsTests ServerDiscoveryTests
QT_QPA_PLATFORM=offscreen ctest --test-dir "$build_dir/src/unittests" \
  -R '^(LocalCursorTests|FileRelayTests|FileTransferTests|ServerProxyTests|ClientProxyTests|ServerTests|SettingsTests|ServerDiscoveryTests)$' --output-on-failure
bundle="$build_dir/bin/Deskflow.app"
translations="$bundle/Contents/Resources/translations"
mkdir -p "$translations"
lrelease "$source_root/translations/deskflow_en.ts" -qm "$translations/deskflow_en.qm"
lrelease "$source_root/translations/deskflow_zh_CN.ts" -qm "$translations/deskflow_zh_CN.qm"
cp "$qt_translations/qtbase_zh_CN.qm" "$translations/qt_zh_CN.qm"
cp "$source_root/LICENSE" "$bundle/Contents/Resources/LICENSE"
cp "$source_root/LICENSES/LicenseRef-OpenSSL-Exception.txt" "$bundle/Contents/Resources/LICENSE_EXCEPTION"
macdeployqt "$bundle" "-executable=$bundle/Contents/MacOS/deskflow-core" "-libpath=$qt_svg/lib" -always-overwrite -codesign=-
codesign --verify --deep --strict "$bundle"
dmg_stage="$build_dir/dmg-stage"
mkdir -p "$dmg_stage"
ditto "$bundle" "$dmg_stage/Deskflow.app"
ln -sfn /Applications "$dmg_stage/Applications"
dmg="$build_dir/Deskflow-LBM-pairing-macOS-$(uname -m).dmg"
hdiutil create -volname "Deskflow LBM" -srcfolder "$dmg_stage" -ov -format UDZO "$dmg"
hdiutil verify "$dmg"
shasum -a 256 "$dmg" > "$dmg.sha256"
printf '\nBuild complete: %s\nQuit the installed Deskflow before installing this app.\n' "$dmg"
