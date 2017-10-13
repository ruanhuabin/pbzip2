Name:		pbzip2
Version:	1.1.10
Release:	1%{?dist}
Summary:	Parallel implementation of bzip2
URL:		http://www.compression.ca/pbzip2/
License:	BSD
Group:		Applications/File
BuildRoot:	%{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)
%if %{?suse_version:1}0
BuildRequires:  bzip2
%else
BuildRequires:  bzip2-devel
%endif
Source0:	http://www.compression.ca/pbzip2/%{name}-%{version}.tar.gz
Patch0:		%{name}-rpm-buildflags.patch

%description
PBZIP2 is a parallel implementation of the bzip2 block-sorting file
compressor that uses pthreads and achieves near-linear speedup on SMP
machines.  The output of this version is fully compatible with bzip2
v1.0.2 or newer (ie: anything compressed with pbzip2 can be 
decompressed with bzip2).


%prep
%setup -q
%patch0 -p1


%build
CXXFLAGS="%{optflags}" CFLAGS="%{optflags}" LDFLAGS="%{?__global_ldflags}" make %{?_smp_mflags}


%install
rm -rf %{buildroot}
install -D -m755 %{name} %{buildroot}%{_bindir}/%{name}
install -D -m644 %{name}.1 %{buildroot}%{_mandir}/man1/%{name}.1
ln -sf ./%{name} %{buildroot}%{_bindir}/pbunzip2
ln -sf ./%{name} %{buildroot}%{_bindir}/pbzcat


%clean
rm -rf %{buildroot}


%files
%defattr(-,root,root)
%doc AUTHORS ChangeLog COPYING README
%{_bindir}/%{name}
%{_bindir}/pbunzip2
%{_bindir}/pbzcat
%{_mandir}/man1/*


%changelog
* Fri Dec 18 2015 Jeff Gilchrist <pbzip2@compression.ca> - 1.1.13-1
- Release 1.1.13

* Sun Dec 21 2014 Jeff Gilchrist <pbzip2@compression.ca> - 1.1.12-1
- Release 1.1.12

* Fri Dec 12 2014 Jeff Gilchrist <pbzip2@compression.ca> - 1.1.11-1
- Release 1.1.11

* Sun Nov 23 2014 Jeff Gilchrist <pbzip2@compression.ca> - 1.1.10-1
- Release 1.1.10

* Sun Apr 13 2014 Jeff Gilchrist <pbzip2@compression.ca> - 1.1.9-1
- Release 1.1.9
- spec file refinement for rpm builds thanks to to Ville Skytta
- Makefile refinements for 

* Sun Jun 10 2012 Jeff Gilchrist <pbzip2@compression.ca> - 1.1.8-1
- Release 1.1.8

* Sun Dec 11 2011 Jeff Gilchrist <pbzip2@compression.ca> - 1.1.7-1
- Release 1.1.7

* Sun Oct 30 2011 Jeff Gilchrist <pbzip2@compression.ca> - 1.1.6-1
- Release 1.1.6

* Sat Jul 16 2011 Jeff Gilchrist <pbzip2@compression.ca> - 1.1.5-1
- Release 1.1.5

* Sat Apr 22 2011 Jeff Gilchrist <pbzip2@compression.ca> - 1.1.4-1
- Release 1.1.4

* Sun Mar 27 2011 Jeff Gilchrist <pbzip2@compression.ca> - 1.1.3-1
- Release 1.1.3

* Sat Feb 19 2011 Jeff Gilchrist <pbzip2@compression.ca> - 1.1.2-1
- Release 1.1.2

* Sat Apr 17 2010 Jeff Gilchrist <pbzip2@compression.ca> - 1.1.1-1
- Release 1.1.1

* Sat Mar 13 2010 Jeff Gilchrist <pbzip2@compression.ca> - 1.1.0-1
- Release 1.1.0

* Fri Jan 8 2009 Jeff Gilchrist <pbzip2@compression.ca> - 1.0.5-1
- Release 1.0.5

* Fri Dec 21 2008 Jeff Gilchrist <pbzip2@compression.ca> - 1.0.4-1
- Release 1.0.4

* Tue Oct 31 2008 Jeff Gilchrist <pbzip2@compression.ca> - 1.0.3-1
- Release 1.0.3
- Added support for SUSE RPM build
- Added symlink for pbzcat

* Thu Jul 26 2007 Jeff Gilchrist <pbzip2@compression.ca> - 1.0.2-2
- Fixed symbolic link for pbunzip2 file

* Tue Jul 25 2007 Jeff Gilchrist <pbzip2@compression.ca> - 1.0.2-1
- Release 1.0.2

* Tue Mar 20 2007 Jeff Gilchrist <pbzip2@compression.ca> - 1.0.1-1
- Release 1.0.1

* Wed Mar 14 2007 Jeff Gilchrist <pbzip2@compression.ca> - 1.0-1
- Release 1.0

* Tue Sep 12 2006 Jeff Gilchrist <pbzip2@compression.ca> - 0.9.6-4
- Rebuild for Fedora Extras 6

* Tue May 23 2006 Jeff Gilchrist <pbzip2@compression.ca> - 0.9.6-3
- Added support for $RPM_OPT_FLAGS thanks to Ville Skytta

* Tue Feb 28 2006 Jeff Gilchrist <pbzip2@compression.ca> - 0.9.6-2
- Rebuild for Fedora Extras 5

* Sun Feb 5 2006 Jeff Gilchrist <pbzip2@compression.ca> - 0.9.6-1
- Release 0.9.6

* Sat Dec 31 2005 Jeff Gilchrist <pbzip2@compression.ca> - 0.9.5-1
- Release 0.9.5

* Tue Aug 30 2005 Jeff Gilchrist <pbzip2@compression.ca> - 0.9.4-1
- Updated RPM spec with suggestions from Oliver Falk

* Fri Jul 29 2005 Bryan Stillwell <bryan@bokeoa.com> - 0.9.3-1
- Release 0.9.3
- Removed non-packaging changelog info
- Added dist macro to release field
- Clean buildroot at the beginning of the install section
- Modified buildroot tag to match with Fedora PackagingGuidelines
- Shortened Requires and BuildRequires list
- Changed description to match with the Debian package

* Sat Mar 12 2005 Jeff Gilchrist <pbzip2@compression.ca> - 0.9.2-1
- Release 0.9.2

* Sat Jan 29 2005 Jeff Gilchrist <pbzip2@compression.ca> - 0.9.1-1
- Release 0.9.1

* Sun Jan 24 2005 Jeff Gilchrist <pbzip2@compression.ca> - 0.9-1
- Release 0.9

* Sun Jan 9 2005 Jeff Gilchrist <pbzip2@compression.ca> - 0.8.3-1
- Release 0.8.3

* Mon Nov 30 2004 Jeff Gilchrist <pbzip2@compression.ca> - 0.8.2-1
- Release 0.8.2

* Sat Nov 27 2004 Jeff Gilchrist <pbzip2@compression.ca> - 0.8.1-1
- Release 0.8.1

* Thu Oct 28 2004 Bryan Stillwell <bryan@bokeoa.com> - 0.8-1
- Initial packaging
