EXTRA_DIST = openssl.conf

AM_CPPFLAGS = \
  -DXRDP_CFG_PATH=\"${sysconfdir}/xrdp\" \
  -DXRDP_SBIN_PATH=\"${sbindir}\" \
  -DXRDP_SHARE_PATH=\"${datadir}/xrdp\" \
  -DXRDP_PID_PATH=\"${localstatedir}/run\" \
  -I$(top_srcdir)/common

bin_PROGRAMS = \
  xrdp-keygen

xrdp_keygen_SOURCES = keygen.c

xrdp_keygen_LDADD = \
  $(top_builddir)/common/libcommon.la

xrdpsysconfdir = $(sysconfdir)/xrdp

install-data-hook:

uninstall-hook:
	rm -f $(DESTDIR)$(xrdpsysconfdir)/rsakeys.ini
	rm -f $(DESTDIR)$(xrdpsysconfdir)/cert.pem
	rm -f $(DESTDIR)$(xrdpsysconfdir)/key.pem
