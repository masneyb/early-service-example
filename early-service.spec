# SPDX-License-Identifier: Apache-2.0

Name:		early-service
Version:	0.1
Release:	1%{?dist}
Summary:	Example service that starts early in the initrd and root filesystems.
License:	Apache-2.0
Source0:	%{name}-%{version}.tar.gz
Requires:	systemd
BuildRequires:	meson
BuildRequires:	gcc
BuildRequires:	glibc-devel
BuildRequires:  systemd-rpm-macros
%{?systemd_requires}
%{?sysusers_requires_compat}

%define dracut_module_destdir /usr/lib/dracut/modules.d/99%{name}

%description
A sample program that runs early in the initrd, and passes it's current state
to a new process started from the root filesystem.

%prep
%autosetup -n %{name}-%{version}

%build
%meson
%meson_build

%install
%meson_install

mkdir -p %{buildroot}%{_unitdir} \
	%{buildroot}%{_sysusersdir} \
	%{buildroot}%{dracut_module_destdir}
install -m644 conf/%{name}-initrd.service %{buildroot}%{_unitdir}/%{name}-initrd.service
install -m644 conf/%{name}.service %{buildroot}%{_unitdir}/%{name}.service
install -m644 conf/module-setup.sh %{buildroot}%{dracut_module_destdir}/module-setup.sh
install -m644 conf/%{name}.sysusers %{buildroot}%{_sysusersdir}/%{name}.conf

%pre
%sysusers_create_compat conf/%{name}.sysusers

%post
%systemd_post %{name}.service
%systemd_post %{name}-initrd.service

%preun
%systemd_preun %{name}.service
%systemd_preun %{name}-initrd.service

%postun
%systemd_postun_with_restart %{name}.service
%systemd_postun_with_restart %{name}-initrd.service

%files
%license LICENSE.txt
%{_bindir}/%{name}
%{_unitdir}/%{name}.service
%{_unitdir}/%{name}-initrd.service
%{_sysusersdir}/%{name}.conf
%dir %{dracut_module_destdir}
%{dracut_module_destdir}/module-setup.sh

%changelog
* Thu Mar 28 2024 Brian Masney <bmasney@redhat.com> - 0.1-1
- Initial public package release
