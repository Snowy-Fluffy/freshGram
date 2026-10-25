%global debug_package %{nil}
%global _build_id_links none
%global __strip /bin/true
%define __os_install_post %{nil}
%define _binary_payload w6.xzdio

Name:           freshgram
Version:        %{freshgram_version}
Release:        1
Summary:        Telegram Desktop fork with the AyuGram features and a Material look
License:        GPL-3.0-or-later
URL:            https://github.com/Snowy-Fluffy/freshGram
BuildArch:      %{freshgram_arch}
AutoReqProv:    no
Requires:       glibc, libstdc++, hicolor-icon-theme
Recommends:     xdg-desktop-portal

%description
freshGram is a Telegram Desktop client that keeps deleted and edited
messages, adds a ghost mode and secret chats, and restyles the interface.

%install
mkdir -p %{buildroot}
cp -a %{freshgram_root}/usr %{buildroot}/usr
(cd %{buildroot} && find usr \( -type f -o -type l \) | sed 's|^|/|') > %{_builddir}/files.list

%files -f %{_builddir}/files.list
%defattr(-,root,root,-)

%post
if command -v update-desktop-database >/dev/null 2>&1; then
	update-desktop-database -q /usr/share/applications || :
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
	gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || :
fi

%postun
if command -v update-desktop-database >/dev/null 2>&1; then
	update-desktop-database -q /usr/share/applications || :
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
	gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || :
fi
