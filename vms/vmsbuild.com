$! vmsbuild.com -- Commands to build GAWK		Pat Rankin, Dec'89
$!							   revised, Mar'90
$!						gawk 2.13  revised, Jun'91
$!						gawk 2.14  revised, Sep'92
$!						gawk 2.15  revised, Oct'93
$!						gawk 3.0   revised, Dec'95
$!						gawk 3.0.1 revised, Nov'96
$!						gawk 3.1.0 revised, Mar'01
$!						gawk 3.1.1 revised, Apr'02
$!						gawk 3.1.6 revised, Mar'07
$!						gawk-bytecode revd, Jan'10
$!						gawk 4.0.0 revd, May'11
$!						gawk 4.1.0 revd, Nov'12
$!
$ REL = "4.1"	!release version number
$ PATCHLVL = "0"
$!
$ if (f$getsyi("HW_MODEL") .lt. 1024)
$ then
$   arch_name = "VAX"
$ else
$   arch_name = f$edit(f$getsyi("ARCH_NAME"), "UPCASE")
$ endif
$!
$ CCFLAGS = "/noList"	! "/noOpt/Debug"
$! CCFLAGS = "/list/show=(expan,incl)
$ CDEFS	  = "GAWK,HAVE_CONFIG_H"
$! Do not specify _POSIX_EXIT here, we are using other tricks for that.
$!
$!
$ if p1.eqs."" then  p1 = "DECC"	!default compiler
$ if p1.eqs."GNUC"
$ then
$! assumes VAX
$   CC = "gcc"
$   if f$type(gcc).eqs."STRING" then  CC = gcc
$   CFLAGS = "/Incl=([],[.vms])/Obj=[]/Def=(''CDEFS')''CCFLAGS'"
$   LIBS = "gnu_cc:[000000]gcclib.olb/Library,sys$library:vaxcrtl.olb/Library"
$   if p2.eqs."DO_GNUC_SETUP" then  set command gnu_cc:[000000]gcc
$ else	!!GNUC
$  if p1.eqs."VAXC"
$  then
$!  always VAX; version V3.x of VAX C assumed (for V2.x, remove /Opt=noInline)
$   CC = "cc"
$   if f$trnlnm("DECC$CC_DEFAULT").nes."" then  CC = "cc/VAXC"
$   CFLAGS = "/Incl=[]/Obj=[]/Opt=noInline/Def=(''CDEFS')''CCFLAGS'"
$   LIBS = "sys$share:vaxcrtl.exe/Shareable"
$  else  !!VAXC
$!  neither GNUC nor VAXC, assume DECC (same for either VAX or Alpha)
$ if arch_name .eqs. "vax"
$ then
$   cfloat = ""
$ else
$   cfloat = "/float=ieee/ieee_mode=denorm_results"
$ endif
$   CC = "cc/DECC/Prefix=All"
$   CINC = "/NESTED_INCLUDE=NONE"
$   CFLAGS = "/Incl=([],[.vms])/Obj=[]/Def=(''CDEFS')''CINC'''CCFLAGS'"
$   CFLAGS = cfloat + CFLAGS
$   LIBS = ""	! DECC$SHR instead of VAXCRTL, no special link option needed
$  endif !VAXC
$ endif !GNUC
$!
$ cc = CC + CFLAGS
$ show symbol cc
$!
$ if f$search("config.h") .nes. ""
$ then
$    if f$cvtime(f$file_attr("config.h", "RDT")) .ges. -
        f$cvtime(f$file_attr("configh.in","RDT")) then  goto config_ok
$ endif
$ v = f$verify(0)
$ @[.vms]generate_config_vms_h_gawk.com
$ @[.vms]config_h.com NOBUILTINS
$!
$config_ok:
$ if f$search("awkgram.c").nes."" then  goto awkgram_ok
$	write sys$output " You must process `awkgram.y' with ""yacc"" or ""bison"""
$	if f$search("awkgram_tab.c").nes."" then -	!bison was run manually
	  write sys$output " or else rename `awkgram_tab.c' to `awkgram.c'."
$	if f$search("ytab.c").nes."" .or. f$search("y_tab.c").nes."" then - !yacc
	  write sys$output " or else rename `ytab.c' or `y_tab.c' to `awkgram.c'."
$	exit
$awkgram_ok:
$ if f$search("command.c").nes."" then  goto command_ok
$	write sys$output " You must process `command.y' with ""yacc"" or ""bison"""
$	if f$search("command_tab.c").nes."" then -	!bison was run manually
	  write sys$output " or else rename `command_tab.c' to `command.c'."
$	if f$search("ytab.c").nes."" .or. f$search("y_tab.c").nes."" then - !yacc
	  write sys$output " or else rename `ytab.c' or `y_tab.c' to `command.c'."
$	exit
$command_ok:
$ v = f$verify(1)
$ cc array.c
$ cc awkgram.c
$ cc builtin.c
$ cc dfa.c
$ cc ext.c
$ cc field.c
$ cc floatcomp.c
$ cc gawkmisc.c
$ cc getopt.c
$ cc getopt1.c
$ cc io.c
$ cc main.c
$ cc msg.c
$ cc node.c
$ cc random.c
$ cc re.c
$ cc regex.c
$ cc replace.c
$ cc version.c
$ cc eval.c
$ cc profile.c
$ cc command.c
$ cc debug.c
$ cc int_array.c
$ cc cint_array.c
$ cc gawkapi.c
$ cc mpfr.c
$ cc str_array.c
$ cc symbol.c
$ cc [.vms]vms_misc.c
$ cc [.vms]vms_popen.c
$ cc [.vms]vms_fwrite.c
$ cc [.vms]vms_args.c
$ cc [.vms]vms_gawk.c
$ cc [.vms]vms_cli.c
$ cc [.vms]vms_crtl_init.c
$ set command/Object=[]gawk_cmd.obj sys$disk:[.vms]gawk.cld
$! 'f$verify(v)'
$!
$ close/noLog Fopt
$ create gawk.opt
! GAWK -- GNU awk
array.obj,awkgram.obj,builtin.obj,dfa.obj,ext.obj,field.obj,floatcomp.obj
gawkmisc.obj,getopt.obj,getopt1.obj,io.obj
main.obj,msg.obj,node.obj
random.obj,re.obj,regex.obj,replace.obj,version.obj,eval.obj,profile.obj
command.obj,debug.obj,int_array.obj,cint_array.obj,gawkapi.obj,mpfr.obj
str_array.obj,symbol.obj
[]vms_misc.obj,vms_popen.obj,vms_fwrite.obj,vms_args.obj
[]vms_gawk.obj,vms_cli.obj,gawk_cmd.obj,vms_crtl_init.obj
psect_attr=environ,noshr	!extern [noshare] char **
stack=48	!preallocate more pages (default is 20)
iosegment=128	!ditto (default is 32)
$!
$ v = f$verify(1)
$ @[.vms]gawk_ident.com
$ open/append Fopt gawk.opt
$ write Fopt libs
$ close Fopt
$!
$ v = f$verify(1)
$ link/exe=gawk.exe gawk.opt/options
$! 'f$verify(v)'
$ exit
