dnl This program is free software; you can redistribute it and/or modify
dnl it under the terms of the GNU General Public License as published by
dnl the Free Software Foundation; either version 2 of the License, or
dnl (at your option) any later version.
dnl
dnl This program is distributed in the hope that it will be useful,
dnl but WITHOUT ANY WARRANTY; without even the implied warranty of
dnl MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
dnl GNU General Public License for more details.
dnl
dnl You should have received a copy of the GNU General Public License
dnl along with this program; if not, write to the Free Software
dnl Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307, USA.
dnl
dnl Process this file with autoconf to produce a configure script.

AC_INIT([mod_gss],[1.3.9a],[markus_moeller@compuserve.com])

AC_TRY_COMPILE([int main()
                 {
                  return;
                 }	
              ])

unset krb5confpath
AC_ARG_WITH(krb5-config,[  --with-krb5-config=PATH specify path to krb5-config @ <: @default=detect@: >@],
	     [if test x"$withval" != xno; then
                 krb5confpath=$withval 
	      fi
	     ])

unset krb5fail 
if test x"$krb5confpath" != xno; then 
	if test x"$krb5confpath" = x ; then 
		AC_CHECK_PROG(ac_krb5_config, krb5-config, yes, no) 
		if test x"$ac_krb5_config" = xyes; then 
			krb5confpath=`which krb5-config`
		fi
	fi
	if test x"$krb5confpath" != x ; then
		if ! test -x "$krb5confpath"; then
			AC_MSG_WARN([krb5-config '$krb5confpath' not executable, ignoring])
		fi
		ac_krb5_solaris="`$krb5confpath --version 2>/dev/null | grep -i solaris`"
		ac_krb5_heimdal="`$krb5confpath --version 2>/dev/null | grep -i heimdal`"
        	if test "x$ac_krb5_solaris" == x ; then
			ac_krb5_cflags=`$krb5confpath --cflags krb5 2> /dev/null `
			if test $? != 0; then
				krb5fail=t
			fi
			ac_krb5_libs=`$krb5confpath --libs krb5 2> /dev/null `
			if test $? != 0; then
				krb5fail=t
			fi
			ac_gssapi_cflags=`$krb5confpath --cflags gssapi 2> /dev/null ` 
			if test $? != 0; then
				krb5fail=t
			fi
	   	    	ac_gssapi_libs=`$krb5confpath --libs gssapi 2> /dev/null ` 
			if test $? != 0; then 
				krb5fail=t 
			fi 
		fi
	fi
fi 

if test x"$krb5fail" = xt ; then
	echo "No GSSAPI/Kerberos support - missing/not working krb5-config"
	exit 0
fi

o_CPPFLAGS=${CPPFLAGS}
o_CFLAGS=${CFLAGS}
o_LDFLAGS=${LDFLAGS}
CPPFLAGS="${CPPFLAGS} $ac_gssapi_cflags $ac_krb5_cflags"
CFLAGS="${CFLAGS} $ac_gssapi_cflags $ac_krb5_cflags"
LDFLAGS="${LDFLAGS} $ac_gssapi_libs $ac_krb5_libs"

dnl look for gssapi headers
AC_CHECK_HEADERS(gssapi.h gssapi/gssapi.h)
AC_CHECK_HEADERS([gssapi/gssapi_ext.h gssapi/gssapi_krb5.h gssapi/gssapi_generic.h], [], [],
[[#ifdef HAVE_GSSAPI_H
#include <gssapi.h>
#elif defined(HAVE_GSSAPI_GSSAPI_H)
#include <gssapi/gssapi.h>
#endif]])

dnl look for kerberos headers
AC_CHECK_HEADERS(kerberosv5/krb5.h krb5.h com_err.h et/com_err.h)
AC_CHECK_HEADERS([krb5-protos.h], [], [], 
[[#ifdef HAVE_KERBEROSV5_KRB5_H
#include <kerberosv5/krb5.h>
#elif HAVE_KRB5_H
#include <krb5.h>
#endif ]] )

AC_MSG_CHECKING([for _krb5_principal2principalname in krb5-private.h])
AC_EGREP_HEADER(_krb5_principal2principalname,krb5-private.h,AC_DEFINE(HAVE_KRB5_PRINCIPAL2PRINCIPAL,1,[Define to have _krb5_principal2principalname in krb5-private.h])
AC_MSG_RESULT(yes),
AC_MSG_RESULT(no))

ac_com_error_message=no
AC_EGREP_HEADER(com_err.h,krb5.h,ac_com_err_krb5=yes)
if test x"$ac_com_err_krb5" = xyes ; then
	AC_DEFINE(HAVE_COM_ERR_IN_KRB5,1,[Define to 1 if you have com_err in krb5.h])
fi
if test x"$ac_cv_header_com_err_h" = xyes ; then
	AC_EGREP_HEADER(error_message,com_err.h,ac_com_error_message=yes)
elif test "x$ac_cv_header_et_com_err_h" = xyes ; then
	AC_EGREP_HEADER(error_message,et/com_err.h,ac_com_error_message=yes)
fi
							       
dnl look for libs
if test x"$ac_gssapi_libs" != x; then 
	LIBS="${LIBS} $ac_gssapi_libs $ac_krb5_libs"
else
	AC_CHECK_LIB(crypto, main)
	AC_CHECK_LIB(des, main)
	AC_CHECK_LIB(crypt, main)
	AC_CHECK_LIB(roken, main)
	AC_CHECK_LIB(com_err, main)
	AC_CHECK_LIB(des425, main)
	AC_CHECK_LIB(k5crypto, main)
	AC_CHECK_LIB(asn1, main)
	AC_CHECK_LIB(krb5, main)
	AC_CHECK_LIB(gssapi_krb5, main)
	AC_CHECK_LIB(gssapi, main)
	AC_CHECK_LIB(gss, main)
	AC_CHECK_LIB(ksvc, main)
fi 
if test `echo $LIBS | grep -c com_err` -ne 0 -a "x$ac_com_error_message" = xyes ; then
	AC_CHECK_LIB(com_err,error_message,
		     AC_DEFINE(HAVE_ERROR_MESSAGE,1,[Define to 1 if you have error_message]),)
elif test  x"$ac_com_error_message" = xyes ; then
	AC_CHECK_LIB(krb5,error_message,
		     AC_DEFINE(HAVE_ERROR_MESSAGE,1,[Define to 1 if you have error_message]),)
fi
AC_CHECK_LIB(krb5,krb5_get_err_text,
	     AC_DEFINE(HAVE_KRB5_GET_ERR_TEXT,1,[Define to 1 if you have krb5_get_err_text]),)
AC_CHECK_LIB(krb5,krb5_get_error_message,
	     AC_DEFINE(HAVE_KRB5_GET_ERROR_MESSAGE,1,[Define to 1 if you have krb5_get_error_message]),)
AC_CHECK_LIB(gss,__gss_userok,AC_DEFINE(HAVE___GSS_USEROK,1,[Define __gss_userok]),)
AC_CHECK_LIB(krb5,krb5_get_init_creds_password,
	     AC_DEFINE(HAVE_INIT_CREDS_PASSWORD,1,[Define krb5_get_init_creds_password]),)
if test x"$ac_cv_lib_krb5_krb5_get_init_creds_keytab" != xyes ; then
	AC_CHECK_LIB(mech_krb5,krb5_get_init_creds_password,
	     AC_DEFINE(HAVE_INIT_CREDS_PASSWORD,1,[Define krb5_get_init_creds_password]),) 
fi

dnl do compile check
no_krb5=t
AC_MSG_CHECKING([for working krb5])
AC_TRY_RUN([
#ifdef HAVE_KERBEROSV5_KRB5_H
#include <kerberosv5/krb5.h>
#elif HAVE_KRB5_H
#include <krb5.h>
#endif
#if defined(HAVE_KRB5_PROTOS_H) && defined(HAVE_KRB5_PRINCIPAL2PRINCIPAL)
#include <krb5-protos.h>
krb5_error_code KRB5_LIB_FUNCTION
_krb5_principal2principalname (
        PrincipalName */*p*/,
        const krb5_principal /*from*/);
#endif

int
main(void)
{
	krb5_context context;
	krb5_init_context(&context);
	return 0;
}
], [unset no_krb5
AC_DEFINE(HAVE_KRB5, 1, [KRB5 support])
AC_MSG_RESULT(yes)],
AC_MSG_RESULT(no))

if test x"$no_krb5" = xt ; then
	echo "No GSSAPI/Kerberos support - not working krb5"
	if test x"$krb5confpath" != xno; then
		if test x"$ac_gssapi_libs" != x ; then
		        _optsonly=`echo $ac_gssapi_libs | xargs -n1 | egrep -v '^-l' | xargs echo`
			if test x"$_optsonly" != x ; then
				echo "Try LDFLAGS -Wl,-R<path> with path from $_optsonly" 
			fi
		fi 
	fi 
	exit 1
fi

AC_MSG_CHECKING([for heimdal])
if test "x$ac_krb5_heimdal" != x ; then
	AC_DEFINE(HAVE_HEIMDAL_KERBEROS,1,[Heimdal Kerberos implementation])
	AC_MSG_RESULT(yes)
else
	AC_MSG_RESULT(no)
fi
AC_MSG_CHECKING([for Solaris])
if test "x$ac_krb5_solaris" != x ; then
	AC_DEFINE(HAVE_SOLARIS_KERBEROS,1,[Solaris Kerberos implementation])
	AC_MSG_RESULT(yes)
else
	AC_MSG_RESULT(no)
fi

# unset have_heimdal 
# AC_MSG_CHECKING([for heimdal])
# AC_TRY_LINK([
# #include "krb5.h"], [printf("%s\n", heimdal_version);],
# [AC_DEFINE(HAVE_HEIMDAL_KERBEROS, 1, [Heimdal kerberos implementation])
# AC_MSG_RESULT(yes) 
# have_heimdal=t],
# [AC_MSG_RESULT(no)]) 

if test x"$ac_gssapi_libs" != x; then 
	have_gssapi_krb5="`echo ${LIBS} | grep gssapi_krb5`" 
	if test x"$have_gssapi_krb5" = x; then 
		AC_CHECK_LIB(gssapi_krb5, gsskrb5_register_acceptor_identity, 
			      LIBS="${LIBS} -lgssapi_krb5",) 
	fi 
fi 

AC_MSG_CHECKING([for gss_int32])
AC_TRY_COMPILE([#if HAVE_GSSAPI_H
#include <gssapi.h>
#elif defined(HAVE_GSSAPI_GSSAPI_H)
#include <gssapi/gssapi.h>
#endif], [gss_int32 gint;],
	       [AC_DEFINE(HAVE_GSS_INT32,1,[Define gss_int32])
	       AC_MSG_RESULT(yes)],[AC_MSG_RESULT(no)])

dnl do compile check
no_gssapi=t
AC_MSG_CHECKING([for working gssapi]) 
AC_TRY_RUN([
#if HAVE_GSSAPI_H
#include <gssapi.h>
#elif HAVE_GSSAPI_GSSAPI_H
#include <gssapi/gssapi.h>
#endif /* HAVE_GSSAPI_H */
#if !HAVE_HEIMDAL_KERBEROS
#if HAVE_GSSAPI_GSSAPI_EXT_H
#include <gssapi/gssapi_ext.h>
#endif /* HAVE_GSSAPI_GSSAPI_EXT_H */
#if HAVE_GSSAPI_GSSAPI_KRB5_H
#include <gssapi/gssapi_krb5.h>
#endif /* HAVE_GSSAPI_GSSAPI_KRB5_H */
#if HAVE_GSSAPI_GSSAPI_GENERIC_H
#include <gssapi/gssapi_generic.h>
#endif /* HAVE_GSSAPI_GSSAPI_GENERIC_H */
#endif /* HAVE_HEIMDAL_KERBEROS */
int main(void)
{
	OM_uint32 val;
	gss_OID_set set;
	gss_create_empty_oid_set(&val, &set);
	return 0;
}
], [unset no_gssapi
 AC_MSG_RESULT(yes)],
[AC_MSG_RESULT(no)])

if test x"$no_gssapi" = xt ; then
	echo "No GSSAPI/Kerberos support - not working gssapi"
        if test x"$krb5confpath" != xno; then 
		if test x"$ac_gssapi_libs" != x ; then 
			_optsonly=`echo $ac_gssapi_libs | xargs -n1 | egrep -v '^-l' | xargs echo` 
			if test x"$_optsonly" != x ; then 
				echo "Try LDFLAGS -Wl,-R<path> with path from $_optsonly" 
			fi 
		fi 
	fi 
	exit 1
else
	AC_DEFINE(HAVE_GSSAPI, 1,[GSSAPI support]) 
fi

AC_CHECK_FUNCS(unsetenv setenv)

for file in gssapi.h gssapi/gssapi.h gssapi/gssapi_generic.h; do
	AC_EGREP_HEADER(gss_nt_service_name,[$file],
	break,
	[AC_EGREP_HEADER(gss_nt_service_name, $file, 
			 GSS_C_NT_HOSTBASED_SERVICE,
			[AC_DEFINE(gss_nt_service_name, GSS_C_NT_HOSTBASED_SERVICE,
			[gss_nt_service_name replacement])
			break;])])
done 

AC_CONFIG_HEADER(mod_gss.h)
AH_TOP([/*
* mod_gss - an RFC2228 GSSAPI module for ProFTPD
*
* Copyright (c) 2002-2018 M Moeller <|MAIL|>
* All rights reserved.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307, USA.
*
* As a special exemption, M Moeller gives permission to link this program
* with MIT, Heimdal or other GSS/Kerberos libraries, and distribute
* the resulting executable, without including the source code for
* the Libraries in the source distribution.
*
*/
])

AH_BOTTOM([#include "conf.h"
#include "privs.h"

/* Make sure the version of proftpd is as necessary. */
#if PROFTPD_VERSION_NUMBER < 0x0001030301
# error "ProFTPD 1.3.0rc1 or later required"
#endif

#ifdef HAVE_GSSAPI_H
#include <gssapi.h>
#elif defined(HAVE_GSSAPI_GSSAPI_H)
#include <gssapi/gssapi.h>
#endif /* HAVE_GSSAPI_H */

#ifndef HAVE_HEIMDAL_KERBEROS
#ifdef HAVE_GSSAPI_GSSAPI_EXT_H
#include <gssapi/gssapi_ext.h>
#endif /* HAVE_GSSAPI_GSSAPI_EXT_H */
#ifdef HAVE_GSSAPI_GSSAPI_KRB5_H
#include <gssapi/gssapi_krb5.h>
#endif /* HAVE_GSSAPI_GSSAPI_KRB5_H */
#ifdef HAVE_GSSAPI_GSSAPI_GENERIC_H
#include <gssapi/gssapi_generic.h>
#endif /* HAVE_GSSAPI_GSSAPI_GENERIC_H */
#endif /* HAVE_HEIMDAL_KERBEROS */

#ifndef HAVE_GSS_INT32
#define gss_int32 int32_t
#endif

#ifdef HAVE_KERBEROSV5_KRB5_H
#include <kerberosv5/krb5.h>
#elif HAVE_KRB5_H
#include <krb5.h>
#endif
#if defined(HAVE_KRB5_PROTOS_H) && defined(HAVE_KRB5_PRINCIPAL2PRINCIPAL)
#include <krb5-protos.h>
krb5_error_code KRB5_LIB_FUNCTION
_krb5_principal2principalname (
        PrincipalName */*p*/,
        const krb5_principal /*from*/);
#endif

#ifndef HAVE_COM_ERR_IN_KRB5
#ifdef HAVE_COM_ERR_H
#include <com_err.h>
#elif defined(HAVE_ET_COM_ERR_H)
#include <et/com_err.h>
#endif /* HAVE_COM_ERR_H */
#endif /* !HAVE_COM_ERR_IN_KRB5 */
#if !defined(HAVE_ERROR_MESSAGE) && defined(HAVE_KRB5_GET_ERROR_MESSAGE)
#define error_message(code) krb5_get_error_message(kc, code)
#elif !defined(HAVE_ERROR_MESSAGE) && defined(HAVE_KRB5_GET_ERR_TEXT)
#define error_message(code) krb5_get_err_text(kc, code)
#elif !defined(HAVE_ERROR_MESSAGE)
static char err_code[17];
const char *KRB5_CALLCONV
error_message(long code) {
   snprintf(err_code, 16, "%ld", code);
   return err_code;
}
#endif /* !HAVE_ERROR_MESSAGE */

#ifndef GSS_C_AF_INET6
#define GSS_C_AF_INET6 24
#endif

extern session_t session;
extern xaset_t *server_list;

#ifndef HAVE_UNSETENV
int unsetenv(char *env_name) {
  extern char **environ;
  char **cc;
  int l;
  l=strlen(env_name);
  for (cc=environ;*cc!=NULL;cc++) {
    if (strncmp(env_name,*cc,l)==0 && ((*cc)[l]=='='||(*cc)[l]=='\0')) break;
  } for (; *cc != NULL; cc++) *cc=cc[1];
}
#endif

#ifndef HAVE_SETENV
int setenv(const char *env_name, const char *value, int rewrite)
{
  extern char **environ;
  static char **new_environ;
  char **cc;
  int l,f;

  l=strlen(env_name);
  f=0;
  for (cc=environ;*cc!=NULL;cc++) {
    if (strncmp(env_name,*cc,l)==0 && ((*cc)[l]=='='||(*cc)[l]=='\0') && rewrite) { f=1; break; };
      if (strncmp(env_name,*cc,l)==0 && ((*cc)[l]=='='||(*cc)[l]=='\0') && !rewrite) return 0;
      } for (; *cc != NULL; cc++) *cc=cc[1];
    if (f) {
      /* Reuse cleared space */
      cc--;
      *cc=malloc(strlen(env_name)+strlen(value)+2);
      strcpy(*cc,env_name);
      strcat(*cc,"=");
      strcat(*cc,value);
    } else {
      /* Reclloc space */
      int n=0;
      for (cc=environ;*cc!=NULL;cc++) n++;
        new_environ=realloc(new_environ,sizeof(char *)*(n+1));
        memcpy(new_environ, environ, sizeof(char *)*n);
        new_environ[n]=malloc(strlen(env_name)+strlen(value)+2);
        strcpy(new_environ[n],env_name);
        strcat(new_environ[n],"=");
        strcat(new_environ[n],value);
        environ=new_environ;
    }
}
#endif
																	   /* gss module definitions and variables */

#define MOD_GSS_VERSION         PACKAGE_NAME"/"PACKAGE_VERSION
#define MOD_AUTH_GSS_VERSION    "mod_auth_gss/"PACKAGE_VERSION
#define C_FWCCC   "FWCCC"       /* FW support PORT/EPORT/PASV/EPASV in clear */

/* mod_gss session flags */
#define GSS_SESS_AUTH_OK                0x0001
#define GSS_SESS_ADAT_OK                0x0002
#define GSS_SESS_PBSZ_OK                0x0004
#define GSS_SESS_DATA_WRITE             0x0010
#define GSS_SESS_DATA_WRITE_OK          0x0020
#define GSS_SESS_DATA_READ_OK           0x0040
#define GSS_SESS_DISPATCH               0x0100
#define GSS_SESS_CCC                    0x0200
#define GSS_SESS_FWCCC                  0x0400
#define GSS_SESS_INT_SUP                0x1000
#define GSS_SESS_CONF_SUP               0x2000
#define GSS_AUTH_AUTHORITATIVE          0x4000

#define GSS_SESS_PROT_C                 0x0000
#define GSS_SESS_PROT_S                 0x0001
#define GSS_SESS_PROT_P                 0x0002
#define GSS_SESS_PROT_E                 0x0004

/* mod_gss option flags*/
#define GSS_OPT_ALLOW_CCC               0x0001
#define GSS_OPT_ALLOW_FW_CCC            0x0002
#define GSS_OPT_ALLOW_FW_NAT            0x0004
#define GSS_OPT_REQUIRE_SEQ_PROT        0x0010
#define GSS_OPT_REQUIRE_REP_PROT        0x0020

])
AC_OUTPUT()

echo "configure: updating mod_gss.h"
sed -e "s/|MAIL|/"$PACKAGE_BUGREPORT"/" mod_gss.h > .mod_gss.h.tmp
mv .mod_gss.h.tmp mod_gss.h

echo "configure: updating \$Libraries in mod_gss.c"
if test -e mod_gss.c ; then
	sed -e "s!\$Libraries:.*!\$Libraries: |GSS_LIBS|\$!" mod_gss.c > mod_gss.c.in
	rm mod_gss.c
fi
sed -e "s!|GSS_LIBS|!$LIBS!" mod_gss.c.in > .mod_gss.c.tmp
mv .mod_gss.c.tmp mod_gss.c

echo "configure: updating \$Libraries in mod_auth_gss.c"
if test -e mod_auth_gss.c ; then
	sed -e "s!\$Libraries:.*!\$Libraries: |GSS_LIBS|\$!" mod_auth_gss.c > mod_auth_gss.c.in
	rm mod_auth_gss.c
fi
sed -e "s!|GSS_LIBS|!$LIBS!" mod_auth_gss.c.in > .mod_auth_gss.c.tmp
mv .mod_auth_gss.c.tmp mod_auth_gss.c

CPPFLAGS=${o_CPPFLAGS}
CFLAGS=${o_CFLAGS}
LDFLAGS=${o_LDFLAGS}

if test x"$ac_gssapi_libs" != x ; then
	_optsonly=`echo $ac_gssapi_libs | xargs -n1 | egrep -v '^-l' | xargs echo`
	if test x"$_optsonly" != x ; then
		LDFLAGS="${LDFLAGS} ${_optsonly}"
		echo "Use LDFLAGS $LDFLAGS with proftpd"
	fi
fi

if test x"$ac_gssapi_cflags" != x ; then
	CFLAGS="${CFLAGS} $ac_gssapi_cflags"
	echo "Use CFLAGS $CFLAGS with proftpd"
fi
