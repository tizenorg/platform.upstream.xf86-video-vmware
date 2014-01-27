Name:           xf86-video-vmware
Version:        12.0.2
Release:        0
License:        MIT
Summary:        VMware SVGA video driver for the Xorg X server
Url:            http://xorg.freedesktop.org/
Group:          System/X11/Servers/XF86_4
#Source0:       http://xorg.freedesktop.org/releases/individual/driver/%{name}-%{version}.tar.bz2
Source0:        %{name}-%{version}_007bed3.tar.bz2
Source1001: 	xf86-video-vmware.manifest
BuildRequires:  pkg-config
BuildRequires:  pkgconfig(fontsproto)
BuildRequires:  pkgconfig(pciaccess) >= 0.8.0
BuildRequires:  pkgconfig(randrproto)
BuildRequires:  pkgconfig(renderproto)
BuildRequires:  pkgconfig(resourceproto)
BuildRequires:  pkgconfig(videoproto)
BuildRequires:  pkgconfig(xatracker) >= 0.4.0
BuildRequires:  pkgconfig(xextproto)
BuildRequires:  pkgconfig(xineramaproto)
BuildRequires:  pkgconfig(xorg-macros) >= 1.8
BuildRequires:  pkgconfig(xorg-server) >= 1.0.1
BuildRequires:  pkgconfig(xproto)

%if %{with x}
ExclusiveArch:  %ix86 x86_64
%else
ExclusiveArch:
%endif


%description
vmware is an Xorg driver for VMware virtual video cards.

%prep
%setup -q
cp %{SOURCE1001} .

%build
%configure
make %{?_smp_mflags}

%install
%make_install

%files
%manifest %{name}.manifest
%defattr(-,root,root)
%doc COPYING
%dir %{_libdir}/xorg/modules/drivers
%{_libdir}/xorg/modules/drivers/vmware_drv.so
%{_mandir}/man4/vmware.4%{?ext_man}

%changelog
