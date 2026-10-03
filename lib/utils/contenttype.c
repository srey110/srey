#include "utils/contenttype.h"

#define CT_EXT_LENS 24 // 扩展名定长存，最长的 23 字节 + '\0'
// 全部 MIME 串列在这一处：同一份列表既生成 ct_types 的字段，也生成它的初值。
// 表行只存串在 ct_types 里的偏移，整张表不含指针
#define CT_TYPE_LIST(X) \
    X(text_h323, "text/h323") \
    X(video_3gpp2, "video/3gpp2") \
    X(video_3gpp, "video/3gpp") \
    X(application_x_7z_compressed, "application/x-7z-compressed") \
    X(audio_audible, "audio/audible") \
    X(audio_aac, "audio/aac") \
    X(application_octet_stream, "application/octet-stream") \
    X(audio_vnd_audible_aax, "audio/vnd.audible.aax") \
    X(audio_ac3, "audio/ac3") \
    X(application_msaccess_addin, "application/msaccess.addin") \
    X(application_msaccess, "application/msaccess") \
    X(application_msaccess_cab, "application/msaccess.cab") \
    X(application_msaccess_runtime, "application/msaccess.runtime") \
    X(application_msaccess_webapplication, "application/msaccess.webapplication") \
    X(application_msaccess_ftemplate, "application/msaccess.ftemplate") \
    X(application_internet_property_stream, "application/internet-property-stream") \
    X(text_xml, "text/xml") \
    X(application_x_bridge_url, "application/x-bridge-url") \
    X(audio_vnd_dlna_adts, "audio/vnd.dlna.adts") \
    X(application_postscript, "application/postscript") \
    X(audio_x_aiff, "audio/x-aiff") \
    X(audio_aiff, "audio/aiff") \
    X(application_vnd_adobe_air_application_installer_package_zip, "application/vnd.adobe.air-application-installer-package+zip") \
    X(application_x_mpeg, "application/x-mpeg") \
    X(application_x_ms_application, "application/x-ms-application") \
    X(image_x_jg, "image/x-jg") \
    X(application_xml, "application/xml") \
    X(video_x_ms_asf, "video/x-ms-asf") \
    X(text_plain, "text/plain") \
    X(application_atom_xml, "application/atom+xml") \
    X(audio_basic, "audio/basic") \
    X(video_x_msvideo, "video/x-msvideo") \
    X(application_olescript, "application/olescript") \
    X(application_x_bcpio, "application/x-bcpio") \
    X(image_bmp, "image/bmp") \
    X(audio_x_caf, "audio/x-caf") \
    X(application_vnd_ms_office_calx, "application/vnd.ms-office.calx") \
    X(application_vnd_ms_pki_seccat, "application/vnd.ms-pki.seccat") \
    X(application_x_cdf, "application/x-cdf") \
    X(application_x_x509_ca_cert, "application/x-x509-ca-cert") \
    X(application_x_java_applet, "application/x-java-applet") \
    X(application_x_msclip, "application/x-msclip") \
    X(image_x_cmx, "image/x-cmx") \
    X(image_cis_cod, "image/cis-cod") \
    X(text_x_ms_contact, "text/x-ms-contact") \
    X(application_x_cpio, "application/x-cpio") \
    X(application_x_mscardfile, "application/x-mscardfile") \
    X(application_pkix_crl, "application/pkix-crl") \
    X(application_x_csh, "application/x-csh") \
    X(text_css, "text/css") \
    X(text_csv, "text/csv") \
    X(application_x_director, "application/x-director") \
    X(video_x_dv, "video/x-dv") \
    X(application_x_msdownload, "application/x-msdownload") \
    X(text_dlm, "text/dlm") \
    X(application_msword, "application/msword") \
    X(application_vnd_ms_word_document_macroEnabled_12, "application/vnd.ms-word.document.macroEnabled.12") \
    X(application_vnd_openxmlformats_officedocument_wordprocessingml_document, "application/vnd.openxmlformats-officedocument.wordprocessingml.document") \
    X(application_vnd_ms_word_template_macroEnabled_12, "application/vnd.ms-word.template.macroEnabled.12") \
    X(application_vnd_openxmlformats_officedocument_wordprocessingml_template, "application/vnd.openxmlformats-officedocument.wordprocessingml.template") \
    X(application_x_dvi, "application/x-dvi") \
    X(drawing_x_dwf, "drawing/x-dwf") \
    X(message_rfc822, "message/rfc822") \
    X(application_etl, "application/etl") \
    X(text_x_setext, "text/x-setext") \
    X(application_envoy, "application/envoy") \
    X(application_vnd_fdf, "application/vnd.fdf") \
    X(application_fractals, "application/fractals") \
    X(Application_xml, "Application/xml") \
    X(x_world_x_vrml, "x-world/x-vrml") \
    X(video_x_flv, "video/x-flv") \
    X(application_fsharp_script, "application/fsharp-script") \
    X(image_gif, "image/gif") \
    X(text_x_ms_group, "text/x-ms-group") \
    X(audio_x_gsm, "audio/x-gsm") \
    X(application_x_gtar, "application/x-gtar") \
    X(application_x_gzip, "application/x-gzip") \
    X(application_x_hdf, "application/x-hdf") \
    X(text_x_hdml, "text/x-hdml") \
    X(application_x_oleobject, "application/x-oleobject") \
    X(application_winhlp, "application/winhlp") \
    X(application_mac_binhex40, "application/mac-binhex40") \
    X(application_hta, "application/hta") \
    X(text_x_component, "text/x-component") \
    X(text_html, "text/html") \
    X(text_webviewhtml, "text/webviewhtml") \
    X(image_x_icon, "image/x-icon") \
    X(image_ief, "image/ief") \
    X(application_x_iphone, "application/x-iphone") \
    X(application_x_internet_signup, "application/x-internet-signup") \
    X(application_x_itunes_ipa, "application/x-itunes-ipa") \
    X(application_x_itunes_ipg, "application/x-itunes-ipg") \
    X(application_x_itunes_ipsw, "application/x-itunes-ipsw") \
    X(text_x_ms_iqy, "text/x-ms-iqy") \
    X(application_x_itunes_ite, "application/x-itunes-ite") \
    X(application_x_itunes_itlp, "application/x-itunes-itlp") \
    X(application_x_itunes_itms, "application/x-itunes-itms") \
    X(application_x_itunes_itpc, "application/x-itunes-itpc") \
    X(video_x_ivf, "video/x-ivf") \
    X(application_java_archive, "application/java-archive") \
    X(application_liquidmotion, "application/liquidmotion") \
    X(image_pjpeg, "image/pjpeg") \
    X(application_x_java_jnlp_file, "application/x-java-jnlp-file") \
    X(image_jpeg, "image/jpeg") \
    X(application_x_javascript, "application/x-javascript") \
    X(text_jscript, "text/jscript") \
    X(application_x_latex, "application/x-latex") \
    X(application_windows_library_xml, "application/windows-library+xml") \
    X(application_x_ms_reader, "application/x-ms-reader") \
    X(video_x_la_asf, "video/x-la-asf") \
    X(application_x_msmediaview, "application/x-msmediaview") \
    X(video_mpeg, "video/mpeg") \
    X(video_vnd_dlna_mpeg_tts, "video/vnd.dlna.mpeg-tts") \
    X(audio_x_mpegurl, "audio/x-mpegurl") \
    X(audio_m4a, "audio/m4a") \
    X(audio_m4b, "audio/m4b") \
    X(audio_m4p, "audio/m4p") \
    X(audio_x_m4r, "audio/x-m4r") \
    X(video_x_m4v, "video/x-m4v") \
    X(image_x_macpaint, "image/x-macpaint") \
    X(application_x_troff_man, "application/x-troff-man") \
    X(application_x_ms_manifest, "application/x-ms-manifest") \
    X(application_x_msaccess, "application/x-msaccess") \
    X(application_x_troff_me, "application/x-troff-me") \
    X(application_x_shockwave_flash, "application/x-shockwave-flash") \
    X(audio_mid, "audio/mid") \
    X(application_x_smaf, "application/x-smaf") \
    X(application_x_msmoney, "application/x-msmoney") \
    X(video_quicktime, "video/quicktime") \
    X(video_x_sgi_movie, "video/x-sgi-movie") \
    X(audio_mpeg, "audio/mpeg") \
    X(video_mp4, "video/mp4") \
    X(application_vnd_ms_mediapackage, "application/vnd.ms-mediapackage") \
    X(application_vnd_ms_project, "application/vnd.ms-project") \
    X(application_x_troff_ms, "application/x-troff-ms") \
    X(application_x_miva_compiled, "application/x-miva-compiled") \
    X(application_x_mmxp, "application/x-mmxp") \
    X(application_x_netcdf, "application/x-netcdf") \
    X(application_oda, "application/oda") \
    X(text_x_ms_odc, "text/x-ms-odc") \
    X(application_vnd_oasis_opendocument_presentation, "application/vnd.oasis.opendocument.presentation") \
    X(application_oleobject, "application/oleobject") \
    X(application_vnd_oasis_opendocument_text, "application/vnd.oasis.opendocument.text") \
    X(application_onenote, "application/onenote") \
    X(application_opensearchdescription_xml, "application/opensearchdescription+xml") \
    X(application_pkcs10, "application/pkcs10") \
    X(application_x_pkcs12, "application/x-pkcs12") \
    X(application_x_pkcs7_certificates, "application/x-pkcs7-certificates") \
    X(application_pkcs7_mime, "application/pkcs7-mime") \
    X(application_x_pkcs7_certreqresp, "application/x-pkcs7-certreqresp") \
    X(application_pkcs7_signature, "application/pkcs7-signature") \
    X(image_x_portable_bitmap, "image/x-portable-bitmap") \
    X(application_x_podcast, "application/x-podcast") \
    X(image_pict, "image/pict") \
    X(application_pdf, "application/pdf") \
    X(image_x_portable_graymap, "image/x-portable-graymap") \
    X(application_vnd_ms_pki_pko, "application/vnd.ms-pki.pko") \
    X(audio_scpls, "audio/scpls") \
    X(application_x_perfmon, "application/x-perfmon") \
    X(image_png, "image/png") \
    X(image_x_portable_anymap, "image/x-portable-anymap") \
    X(application_vnd_ms_powerpoint, "application/vnd.ms-powerpoint") \
    X(application_vnd_ms_powerpoint_template_macroEnabled_12, "application/vnd.ms-powerpoint.template.macroEnabled.12") \
    X(application_vnd_openxmlformats_officedocument_presentationml_template, "application/vnd.openxmlformats-officedocument.presentationml.template") \
    X(application_vnd_ms_powerpoint_addin_macroEnabled_12, "application/vnd.ms-powerpoint.addin.macroEnabled.12") \
    X(image_x_portable_pixmap, "image/x-portable-pixmap") \
    X(application_vnd_ms_powerpoint_slideshow_macroEnabled_12, "application/vnd.ms-powerpoint.slideshow.macroEnabled.12") \
    X(application_vnd_openxmlformats_officedocument_presentationml_slideshow, "application/vnd.openxmlformats-officedocument.presentationml.slideshow") \
    X(application_vnd_ms_powerpoint_presentation_macroEnabled_12, "application/vnd.ms-powerpoint.presentation.macroEnabled.12") \
    X(application_vnd_openxmlformats_officedocument_presentationml_presentation, "application/vnd.openxmlformats-officedocument.presentationml.presentation") \
    X(application_pics_rules, "application/pics-rules") \
    X(application_PowerShell, "application/PowerShell") \
    X(application_x_mspublisher, "application/x-mspublisher") \
    X(text_x_html_insertion, "text/x-html-insertion") \
    X(image_x_quicktime, "image/x-quicktime") \
    X(application_x_quicktimeplayer, "application/x-quicktimeplayer") \
    X(audio_x_pn_realaudio, "audio/x-pn-realaudio") \
    X(image_x_cmu_raster, "image/x-cmu-raster") \
    X(application_rat_file, "application/rat-file") \
    X(image_vnd_rn_realflash, "image/vnd.rn-realflash") \
    X(image_x_rgb, "image/x-rgb") \
    X(application_vnd_rn_realmedia, "application/vnd.rn-realmedia") \
    X(application_vnd_rn_rn_music_package, "application/vnd.rn-rn_music_package") \
    X(application_x_troff, "application/x-troff") \
    X(audio_x_pn_realaudio_plugin, "audio/x-pn-realaudio-plugin") \
    X(text_x_ms_rqy, "text/x-ms-rqy") \
    X(application_rtf, "application/rtf") \
    X(text_richtext, "text/richtext") \
    X(application_x_safari_safariextz, "application/x-safari-safariextz") \
    X(application_x_msschedule, "application/x-msschedule") \
    X(text_scriptlet, "text/scriptlet") \
    X(audio_x_sd2, "audio/x-sd2") \
    X(application_sdp, "application/sdp") \
    X(application_windows_search_connector_xml, "application/windows-search-connector+xml") \
    X(application_set_payment_initiation, "application/set-payment-initiation") \
    X(application_set_registration_initiation, "application/set-registration-initiation") \
    X(application_x_sgimb, "application/x-sgimb") \
    X(text_sgml, "text/sgml") \
    X(application_x_sh, "application/x-sh") \
    X(application_x_shar, "application/x-shar") \
    X(application_x_stuffit, "application/x-stuffit") \
    X(application_vnd_ms_powerpoint_slide_macroEnabled_12, "application/vnd.ms-powerpoint.slide.macroEnabled.12") \
    X(application_vnd_openxmlformats_officedocument_presentationml_slide, "application/vnd.openxmlformats-officedocument.presentationml.slide") \
    X(application_vnd_ms_excel, "application/vnd.ms-excel") \
    X(application_x_ms_license, "application/x-ms-license") \
    X(audio_x_smd, "audio/x-smd") \
    X(application_futuresplash, "application/futuresplash") \
    X(application_x_wais_source, "application/x-wais-source") \
    X(application_streamingmedia, "application/streamingmedia") \
    X(application_vnd_ms_pki_certstore, "application/vnd.ms-pki.certstore") \
    X(application_vnd_ms_pki_stl, "application/vnd.ms-pki.stl") \
    X(application_x_sv4cpio, "application/x-sv4cpio") \
    X(application_x_sv4crc, "application/x-sv4crc") \
    X(application_x_tar, "application/x-tar") \
    X(application_x_tcl, "application/x-tcl") \
    X(application_x_tex, "application/x-tex") \
    X(application_x_texinfo, "application/x-texinfo") \
    X(application_x_compressed, "application/x-compressed") \
    X(application_vnd_ms_officetheme, "application/vnd.ms-officetheme") \
    X(image_tiff, "image/tiff") \
    X(application_x_msterminal, "application/x-msterminal") \
    X(text_tab_separated_values, "text/tab-separated-values") \
    X(text_iuls, "text/iuls") \
    X(application_x_ustar, "application/x-ustar") \
    X(text_vbscript, "text/vbscript") \
    X(text_x_vcard, "text/x-vcard") \
    X(application_vnd_ms_visio_viewer, "application/vnd.ms-visio.viewer") \
    X(application_vnd_visio, "application/vnd.visio") \
    X(application_ms_vsi, "application/ms-vsi") \
    X(application_vsix, "application/vsix") \
    X(application_x_ms_vsto, "application/x-ms-vsto") \
    X(audio_wav, "audio/wav") \
    X(audio_x_ms_wax, "audio/x-ms-wax") \
    X(image_vnd_wap_wbmp, "image/vnd.wap.wbmp") \
    X(application_vnd_ms_works, "application/vnd.ms-works") \
    X(image_vnd_ms_photo, "image/vnd.ms-photo") \
    X(application_x_safari_webarchive, "application/x-safari-webarchive") \
    X(application_wlmoviemaker, "application/wlmoviemaker") \
    X(application_x_wlpg_detect, "application/x-wlpg-detect") \
    X(application_x_wlpg3_detect, "application/x-wlpg3-detect") \
    X(video_x_ms_wm, "video/x-ms-wm") \
    X(audio_x_ms_wma, "audio/x-ms-wma") \
    X(application_x_ms_wmd, "application/x-ms-wmd") \
    X(application_x_msmetafile, "application/x-msmetafile") \
    X(text_vnd_wap_wml, "text/vnd.wap.wml") \
    X(application_vnd_wap_wmlc, "application/vnd.wap.wmlc") \
    X(text_vnd_wap_wmlscript, "text/vnd.wap.wmlscript") \
    X(application_vnd_wap_wmlscriptc, "application/vnd.wap.wmlscriptc") \
    X(video_x_ms_wmp, "video/x-ms-wmp") \
    X(video_x_ms_wmv, "video/x-ms-wmv") \
    X(video_x_ms_wmx, "video/x-ms-wmx") \
    X(application_x_ms_wmz, "application/x-ms-wmz") \
    X(application_vnd_ms_wpl, "application/vnd.ms-wpl") \
    X(application_x_mswrite, "application/x-mswrite") \
    X(video_x_ms_wvx, "video/x-ms-wvx") \
    X(application_directx, "application/directx") \
    X(application_xaml_xml, "application/xaml+xml") \
    X(application_x_silverlight_app, "application/x-silverlight-app") \
    X(application_x_ms_xbap, "application/x-ms-xbap") \
    X(image_x_xbitmap, "image/x-xbitmap") \
    X(application_xhtml_xml, "application/xhtml+xml") \
    X(application_vnd_ms_excel_addin_macroEnabled_12, "application/vnd.ms-excel.addin.macroEnabled.12") \
    X(application_vnd_ms_excel_sheet_binary_macroEnabled_12, "application/vnd.ms-excel.sheet.binary.macroEnabled.12") \
    X(application_vnd_ms_excel_sheet_macroEnabled_12, "application/vnd.ms-excel.sheet.macroEnabled.12") \
    X(application_vnd_openxmlformats_officedocument_spreadsheetml_sheet, "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet") \
    X(application_vnd_ms_excel_template_macroEnabled_12, "application/vnd.ms-excel.template.macroEnabled.12") \
    X(application_vnd_openxmlformats_officedocument_spreadsheetml_template, "application/vnd.openxmlformats-officedocument.spreadsheetml.template") \
    X(image_x_xpixmap, "image/x-xpixmap") \
    X(application_vnd_ms_xpsdocument, "application/vnd.ms-xpsdocument") \
    X(image_x_xwindowdump, "image/x-xwindowdump") \
    X(application_x_compress, "application/x-compress") \
    X(application_x_zip_compressed, "application/x-zip-compressed")
#define CT_FIELD(name, str) char name[sizeof(str)];
#define CT_INIT(name, str) str,
#define CT(name) ((uint16_t)offsetof(ct_types, name))

typedef struct ct_types {
    CT_TYPE_LIST(CT_FIELD)
} ct_types;
typedef struct contenttype_ctx {
    char extension[CT_EXT_LENS];
    uint16_t type; // 类型串在 _ct_types 里的偏移
} contenttype_ctx;

static const ct_types _ct_types = { CT_TYPE_LIST(CT_INIT) };
static const contenttype_ctx _typegreg[] = {
    { ".323", CT(text_h323) },
    { ".3g2", CT(video_3gpp2) },
    { ".3gp", CT(video_3gpp) },
    { ".3gp2", CT(video_3gpp2) },
    { ".3gpp", CT(video_3gpp) },
    { ".7z", CT(application_x_7z_compressed) },
    { ".aa", CT(audio_audible) },
    { ".aac", CT(audio_aac) },
    { ".aaf", CT(application_octet_stream) },
    { ".aax", CT(audio_vnd_audible_aax) },
    { ".ac3", CT(audio_ac3) },
    { ".aca", CT(application_octet_stream) },
    { ".accda", CT(application_msaccess_addin) },
    { ".accdb", CT(application_msaccess) },
    { ".accdc", CT(application_msaccess_cab) },
    { ".accde", CT(application_msaccess) },
    { ".accdr", CT(application_msaccess_runtime) },
    { ".accdt", CT(application_msaccess) },
    { ".accdw", CT(application_msaccess_webapplication) },
    { ".accft", CT(application_msaccess_ftemplate) },
    { ".acx", CT(application_internet_property_stream) },
    { ".addin", CT(text_xml) },
    { ".ade", CT(application_msaccess) },
    { ".adobebridge", CT(application_x_bridge_url) },
    { ".adp", CT(application_msaccess) },
    { ".adt", CT(audio_vnd_dlna_adts) },
    { ".adts", CT(audio_aac) },
    { ".afm", CT(application_octet_stream) },
    { ".ai", CT(application_postscript) },
    { ".aif", CT(audio_x_aiff) },
    { ".aifc", CT(audio_aiff) },
    { ".aiff", CT(audio_aiff) },
    { ".air", CT(application_vnd_adobe_air_application_installer_package_zip) },
    { ".amc", CT(application_x_mpeg) },
    { ".application", CT(application_x_ms_application) },
    { ".art", CT(image_x_jg) },
    { ".asa", CT(application_xml) },
    { ".asax", CT(application_xml) },
    { ".ascx", CT(application_xml) },
    { ".asd", CT(application_octet_stream) },
    { ".asf", CT(video_x_ms_asf) },
    { ".ashx", CT(application_xml) },
    { ".asi", CT(application_octet_stream) },
    { ".asm", CT(text_plain) },
    { ".asmx", CT(application_xml) },
    { ".aspx", CT(application_xml) },
    { ".asr", CT(video_x_ms_asf) },
    { ".asx", CT(video_x_ms_asf) },
    { ".atom", CT(application_atom_xml) },
    { ".au", CT(audio_basic) },
    { ".avi", CT(video_x_msvideo) },
    { ".axs", CT(application_olescript) },
    { ".bas", CT(text_plain) },
    { ".bcpio", CT(application_x_bcpio) },
    { ".bin", CT(application_octet_stream) },
    { ".bmp", CT(image_bmp) },
    { ".c", CT(text_plain) },
    { ".cab", CT(application_octet_stream) },
    { ".caf", CT(audio_x_caf) },
    { ".calx", CT(application_vnd_ms_office_calx) },
    { ".cat", CT(application_vnd_ms_pki_seccat) },
    { ".cc", CT(text_plain) },
    { ".cd", CT(text_plain) },
    { ".cdda", CT(audio_aiff) },
    { ".cdf", CT(application_x_cdf) },
    { ".cer", CT(application_x_x509_ca_cert) },
    { ".chm", CT(application_octet_stream) },
    { ".class", CT(application_x_java_applet) },
    { ".clp", CT(application_x_msclip) },
    { ".cmx", CT(image_x_cmx) },
    { ".cnf", CT(text_plain) },
    { ".cod", CT(image_cis_cod) },
    { ".config", CT(application_xml) },
    { ".contact", CT(text_x_ms_contact) },
    { ".coverage", CT(application_xml) },
    { ".cpio", CT(application_x_cpio) },
    { ".cpp", CT(text_plain) },
    { ".crd", CT(application_x_mscardfile) },
    { ".crl", CT(application_pkix_crl) },
    { ".crt", CT(application_x_x509_ca_cert) },
    { ".cs", CT(text_plain) },
    { ".csdproj", CT(text_plain) },
    { ".csh", CT(application_x_csh) },
    { ".csproj", CT(text_plain) },
    { ".css", CT(text_css) },
    { ".csv", CT(text_csv) },
    { ".cur", CT(application_octet_stream) },
    { ".cxx", CT(text_plain) },
    { ".dat", CT(application_octet_stream) },
    { ".datasource", CT(application_xml) },
    { ".dbproj", CT(text_plain) },
    { ".dcr", CT(application_x_director) },
    { ".def", CT(text_plain) },
    { ".deploy", CT(application_octet_stream) },
    { ".der", CT(application_x_x509_ca_cert) },
    { ".dgml", CT(application_xml) },
    { ".dib", CT(image_bmp) },
    { ".dif", CT(video_x_dv) },
    { ".dir", CT(application_x_director) },
    { ".disco", CT(text_xml) },
    { ".dll", CT(application_x_msdownload) },
    { ".dll.config", CT(text_xml) },
    { ".dlm", CT(text_dlm) },
    { ".doc", CT(application_msword) },
    { ".docm", CT(application_vnd_ms_word_document_macroEnabled_12) },
    { ".docx", CT(application_vnd_openxmlformats_officedocument_wordprocessingml_document) },
    { ".dot", CT(application_msword) },
    { ".dotm", CT(application_vnd_ms_word_template_macroEnabled_12) },
    { ".dotx", CT(application_vnd_openxmlformats_officedocument_wordprocessingml_template) },
    { ".dsp", CT(application_octet_stream) },
    { ".dsw", CT(text_plain) },
    { ".dtd", CT(text_xml) },
    { ".dtsconfig", CT(text_xml) },
    { ".dv", CT(video_x_dv) },
    { ".dvi", CT(application_x_dvi) },
    { ".dwf", CT(drawing_x_dwf) },
    { ".dwp", CT(application_octet_stream) },
    { ".dxr", CT(application_x_director) },
    { ".eml", CT(message_rfc822) },
    { ".emz", CT(application_octet_stream) },
    { ".eot", CT(application_octet_stream) },
    { ".eps", CT(application_postscript) },
    { ".etl", CT(application_etl) },
    { ".etx", CT(text_x_setext) },
    { ".evy", CT(application_envoy) },
    { ".exe", CT(application_octet_stream) },
    { ".exe.config", CT(text_xml) },
    { ".fdf", CT(application_vnd_fdf) },
    { ".fif", CT(application_fractals) },
    { ".filters", CT(Application_xml) },
    { ".fla", CT(application_octet_stream) },
    { ".flr", CT(x_world_x_vrml) },
    { ".flv", CT(video_x_flv) },
    { ".fsscript", CT(application_fsharp_script) },
    { ".fsx", CT(application_fsharp_script) },
    { ".generictest", CT(application_xml) },
    { ".gif", CT(image_gif) },
    { ".group", CT(text_x_ms_group) },
    { ".gsm", CT(audio_x_gsm) },
    { ".gtar", CT(application_x_gtar) },
    { ".gz", CT(application_x_gzip) },
    { ".h", CT(text_plain) },
    { ".hdf", CT(application_x_hdf) },
    { ".hdml", CT(text_x_hdml) },
    { ".hhc", CT(application_x_oleobject) },
    { ".hhk", CT(application_octet_stream) },
    { ".hhp", CT(application_octet_stream) },
    { ".hlp", CT(application_winhlp) },
    { ".hpp", CT(text_plain) },
    { ".hqx", CT(application_mac_binhex40) },
    { ".hta", CT(application_hta) },
    { ".htc", CT(text_x_component) },
    { ".htm", CT(text_html) },
    { ".html", CT(text_html) },
    { ".htt", CT(text_webviewhtml) },
    { ".hxa", CT(application_xml) },
    { ".hxc", CT(application_xml) },
    { ".hxd", CT(application_octet_stream) },
    { ".hxe", CT(application_xml) },
    { ".hxf", CT(application_xml) },
    { ".hxh", CT(application_octet_stream) },
    { ".hxi", CT(application_octet_stream) },
    { ".hxk", CT(application_xml) },
    { ".hxq", CT(application_octet_stream) },
    { ".hxr", CT(application_octet_stream) },
    { ".hxs", CT(application_octet_stream) },
    { ".hxt", CT(text_html) },
    { ".hxv", CT(application_xml) },
    { ".hxw", CT(application_octet_stream) },
    { ".hxx", CT(text_plain) },
    { ".i", CT(text_plain) },
    { ".ico", CT(image_x_icon) },
    { ".ics", CT(application_octet_stream) },
    { ".idl", CT(text_plain) },
    { ".ief", CT(image_ief) },
    { ".iii", CT(application_x_iphone) },
    { ".inc", CT(text_plain) },
    { ".inf", CT(application_octet_stream) },
    { ".inl", CT(text_plain) },
    { ".ins", CT(application_x_internet_signup) },
    { ".ipa", CT(application_x_itunes_ipa) },
    { ".ipg", CT(application_x_itunes_ipg) },
    { ".ipproj", CT(text_plain) },
    { ".ipsw", CT(application_x_itunes_ipsw) },
    { ".iqy", CT(text_x_ms_iqy) },
    { ".isp", CT(application_x_internet_signup) },
    { ".ite", CT(application_x_itunes_ite) },
    { ".itlp", CT(application_x_itunes_itlp) },
    { ".itms", CT(application_x_itunes_itms) },
    { ".itpc", CT(application_x_itunes_itpc) },
    { ".ivf", CT(video_x_ivf) },
    { ".jar", CT(application_java_archive) },
    { ".java", CT(application_octet_stream) },
    { ".jck", CT(application_liquidmotion) },
    { ".jcz", CT(application_liquidmotion) },
    { ".jfif", CT(image_pjpeg) },
    { ".jnlp", CT(application_x_java_jnlp_file) },
    { ".jpb", CT(application_octet_stream) },
    { ".jpe", CT(image_jpeg) },
    { ".jpeg", CT(image_jpeg) },
    { ".jpg", CT(image_jpeg) },
    { ".js", CT(application_x_javascript) },
    { ".jsx", CT(text_jscript) },
    { ".jsxbin", CT(text_plain) },
    { ".latex", CT(application_x_latex) },
    { ".library-ms", CT(application_windows_library_xml) },
    { ".lit", CT(application_x_ms_reader) },
    { ".loadtest", CT(application_xml) },
    { ".lpk", CT(application_octet_stream) },
    { ".lsf", CT(video_x_la_asf) },
    { ".lst", CT(text_plain) },
    { ".lsx", CT(video_x_la_asf) },
    { ".lzh", CT(application_octet_stream) },
    { ".m13", CT(application_x_msmediaview) },
    { ".m14", CT(application_x_msmediaview) },
    { ".m1v", CT(video_mpeg) },
    { ".m2t", CT(video_vnd_dlna_mpeg_tts) },
    { ".m2ts", CT(video_vnd_dlna_mpeg_tts) },
    { ".m2v", CT(video_mpeg) },
    { ".m3u", CT(audio_x_mpegurl) },
    { ".m3u8", CT(audio_x_mpegurl) },
    { ".m4a", CT(audio_m4a) },
    { ".m4b", CT(audio_m4b) },
    { ".m4p", CT(audio_m4p) },
    { ".m4r", CT(audio_x_m4r) },
    { ".m4v", CT(video_x_m4v) },
    { ".mac", CT(image_x_macpaint) },
    { ".mak", CT(text_plain) },
    { ".man", CT(application_x_troff_man) },
    { ".manifest", CT(application_x_ms_manifest) },
    { ".map", CT(text_plain) },
    { ".master", CT(application_xml) },
    { ".mda", CT(application_msaccess) },
    { ".mdb", CT(application_x_msaccess) },
    { ".mde", CT(application_msaccess) },
    { ".mdp", CT(application_octet_stream) },
    { ".me", CT(application_x_troff_me) },
    { ".mfp", CT(application_x_shockwave_flash) },
    { ".mht", CT(message_rfc822) },
    { ".mhtml", CT(message_rfc822) },
    { ".mid", CT(audio_mid) },
    { ".midi", CT(audio_mid) },
    { ".mix", CT(application_octet_stream) },
    { ".mk", CT(text_plain) },
    { ".mmf", CT(application_x_smaf) },
    { ".mno", CT(text_xml) },
    { ".mny", CT(application_x_msmoney) },
    { ".mod", CT(video_mpeg) },
    { ".mov", CT(video_quicktime) },
    { ".movie", CT(video_x_sgi_movie) },
    { ".mp2", CT(video_mpeg) },
    { ".mp2v", CT(video_mpeg) },
    { ".mp3", CT(audio_mpeg) },
    { ".mp4", CT(video_mp4) },
    { ".mp4v", CT(video_mp4) },
    { ".mpa", CT(video_mpeg) },
    { ".mpe", CT(video_mpeg) },
    { ".mpeg", CT(video_mpeg) },
    { ".mpf", CT(application_vnd_ms_mediapackage) },
    { ".mpg", CT(video_mpeg) },
    { ".mpp", CT(application_vnd_ms_project) },
    { ".mpv2", CT(video_mpeg) },
    { ".mqv", CT(video_quicktime) },
    { ".ms", CT(application_x_troff_ms) },
    { ".msi", CT(application_octet_stream) },
    { ".mso", CT(application_octet_stream) },
    { ".mts", CT(video_vnd_dlna_mpeg_tts) },
    { ".mtx", CT(application_xml) },
    { ".mvb", CT(application_x_msmediaview) },
    { ".mvc", CT(application_x_miva_compiled) },
    { ".mxp", CT(application_x_mmxp) },
    { ".nc", CT(application_x_netcdf) },
    { ".nsc", CT(video_x_ms_asf) },
    { ".nws", CT(message_rfc822) },
    { ".ocx", CT(application_octet_stream) },
    { ".oda", CT(application_oda) },
    { ".odc", CT(text_x_ms_odc) },
    { ".odh", CT(text_plain) },
    { ".odl", CT(text_plain) },
    { ".odp", CT(application_vnd_oasis_opendocument_presentation) },
    { ".ods", CT(application_oleobject) },
    { ".odt", CT(application_vnd_oasis_opendocument_text) },
    { ".one", CT(application_onenote) },
    { ".onea", CT(application_onenote) },
    { ".onepkg", CT(application_onenote) },
    { ".onetmp", CT(application_onenote) },
    { ".onetoc", CT(application_onenote) },
    { ".onetoc2", CT(application_onenote) },
    { ".orderedtest", CT(application_xml) },
    { ".osdx", CT(application_opensearchdescription_xml) },
    { ".p10", CT(application_pkcs10) },
    { ".p12", CT(application_x_pkcs12) },
    { ".p7b", CT(application_x_pkcs7_certificates) },
    { ".p7c", CT(application_pkcs7_mime) },
    { ".p7m", CT(application_pkcs7_mime) },
    { ".p7r", CT(application_x_pkcs7_certreqresp) },
    { ".p7s", CT(application_pkcs7_signature) },
    { ".pbm", CT(image_x_portable_bitmap) },
    { ".pcast", CT(application_x_podcast) },
    { ".pct", CT(image_pict) },
    { ".pcx", CT(application_octet_stream) },
    { ".pcz", CT(application_octet_stream) },
    { ".pdf", CT(application_pdf) },
    { ".pfb", CT(application_octet_stream) },
    { ".pfm", CT(application_octet_stream) },
    { ".pfx", CT(application_x_pkcs12) },
    { ".pgm", CT(image_x_portable_graymap) },
    { ".pic", CT(image_pict) },
    { ".pict", CT(image_pict) },
    { ".pkgdef", CT(text_plain) },
    { ".pkgundef", CT(text_plain) },
    { ".pko", CT(application_vnd_ms_pki_pko) },
    { ".pls", CT(audio_scpls) },
    { ".pma", CT(application_x_perfmon) },
    { ".pmc", CT(application_x_perfmon) },
    { ".pml", CT(application_x_perfmon) },
    { ".pmr", CT(application_x_perfmon) },
    { ".pmw", CT(application_x_perfmon) },
    { ".png", CT(image_png) },
    { ".pnm", CT(image_x_portable_anymap) },
    { ".pnt", CT(image_x_macpaint) },
    { ".pntg", CT(image_x_macpaint) },
    { ".pnz", CT(image_png) },
    { ".pot", CT(application_vnd_ms_powerpoint) },
    { ".potm", CT(application_vnd_ms_powerpoint_template_macroEnabled_12) },
    { ".potx", CT(application_vnd_openxmlformats_officedocument_presentationml_template) },
    { ".ppa", CT(application_vnd_ms_powerpoint) },
    { ".ppam", CT(application_vnd_ms_powerpoint_addin_macroEnabled_12) },
    { ".ppm", CT(image_x_portable_pixmap) },
    { ".pps", CT(application_vnd_ms_powerpoint) },
    { ".ppsm", CT(application_vnd_ms_powerpoint_slideshow_macroEnabled_12) },
    { ".ppsx", CT(application_vnd_openxmlformats_officedocument_presentationml_slideshow) },
    { ".ppt", CT(application_vnd_ms_powerpoint) },
    { ".pptm", CT(application_vnd_ms_powerpoint_presentation_macroEnabled_12) },
    { ".pptx", CT(application_vnd_openxmlformats_officedocument_presentationml_presentation) },
    { ".prf", CT(application_pics_rules) },
    { ".prm", CT(application_octet_stream) },
    { ".prx", CT(application_octet_stream) },
    { ".ps", CT(application_postscript) },
    { ".psc1", CT(application_PowerShell) },
    { ".psd", CT(application_octet_stream) },
    { ".psess", CT(application_xml) },
    { ".psm", CT(application_octet_stream) },
    { ".psp", CT(application_octet_stream) },
    { ".pub", CT(application_x_mspublisher) },
    { ".pwz", CT(application_vnd_ms_powerpoint) },
    { ".qht", CT(text_x_html_insertion) },
    { ".qhtm", CT(text_x_html_insertion) },
    { ".qt", CT(video_quicktime) },
    { ".qti", CT(image_x_quicktime) },
    { ".qtif", CT(image_x_quicktime) },
    { ".qtl", CT(application_x_quicktimeplayer) },
    { ".qxd", CT(application_octet_stream) },
    { ".ra", CT(audio_x_pn_realaudio) },
    { ".ram", CT(audio_x_pn_realaudio) },
    { ".rar", CT(application_octet_stream) },
    { ".ras", CT(image_x_cmu_raster) },
    { ".rat", CT(application_rat_file) },
    { ".rc", CT(text_plain) },
    { ".rc2", CT(text_plain) },
    { ".rct", CT(text_plain) },
    { ".rdlc", CT(application_xml) },
    { ".resx", CT(application_xml) },
    { ".rf", CT(image_vnd_rn_realflash) },
    { ".rgb", CT(image_x_rgb) },
    { ".rgs", CT(text_plain) },
    { ".rm", CT(application_vnd_rn_realmedia) },
    { ".rmi", CT(audio_mid) },
    { ".rmp", CT(application_vnd_rn_rn_music_package) },
    { ".roff", CT(application_x_troff) },
    { ".rpm", CT(audio_x_pn_realaudio_plugin) },
    { ".rqy", CT(text_x_ms_rqy) },
    { ".rtf", CT(application_rtf) },
    { ".rtx", CT(text_richtext) },
    { ".ruleset", CT(application_xml) },
    { ".s", CT(text_plain) },
    { ".safariextz", CT(application_x_safari_safariextz) },
    { ".scd", CT(application_x_msschedule) },
    { ".sct", CT(text_scriptlet) },
    { ".sd2", CT(audio_x_sd2) },
    { ".sdp", CT(application_sdp) },
    { ".sea", CT(application_octet_stream) },
    { ".searchConnector-ms", CT(application_windows_search_connector_xml) },
    { ".setpay", CT(application_set_payment_initiation) },
    { ".setreg", CT(application_set_registration_initiation) },
    { ".settings", CT(application_xml) },
    { ".sgimb", CT(application_x_sgimb) },
    { ".sgml", CT(text_sgml) },
    { ".sh", CT(application_x_sh) },
    { ".shar", CT(application_x_shar) },
    { ".shtml", CT(text_html) },
    { ".sit", CT(application_x_stuffit) },
    { ".sitemap", CT(application_xml) },
    { ".skin", CT(application_xml) },
    { ".sldm", CT(application_vnd_ms_powerpoint_slide_macroEnabled_12) },
    { ".sldx", CT(application_vnd_openxmlformats_officedocument_presentationml_slide) },
    { ".slk", CT(application_vnd_ms_excel) },
    { ".sln", CT(text_plain) },
    { ".slupkg-ms", CT(application_x_ms_license) },
    { ".smd", CT(audio_x_smd) },
    { ".smi", CT(application_octet_stream) },
    { ".smx", CT(audio_x_smd) },
    { ".smz", CT(audio_x_smd) },
    { ".snd", CT(audio_basic) },
    { ".snippet", CT(application_xml) },
    { ".snp", CT(application_octet_stream) },
    { ".sol", CT(text_plain) },
    { ".sor", CT(text_plain) },
    { ".spc", CT(application_x_pkcs7_certificates) },
    { ".spl", CT(application_futuresplash) },
    { ".src", CT(application_x_wais_source) },
    { ".srf", CT(text_plain) },
    { ".ssisdeploymentmanifest", CT(text_xml) },
    { ".ssm", CT(application_streamingmedia) },
    { ".sst", CT(application_vnd_ms_pki_certstore) },
    { ".stl", CT(application_vnd_ms_pki_stl) },
    { ".sv4cpio", CT(application_x_sv4cpio) },
    { ".sv4crc", CT(application_x_sv4crc) },
    { ".svc", CT(application_xml) },
    { ".swf", CT(application_x_shockwave_flash) },
    { ".t", CT(application_x_troff) },
    { ".tar", CT(application_x_tar) },
    { ".tcl", CT(application_x_tcl) },
    { ".testrunconfig", CT(application_xml) },
    { ".testsettings", CT(application_xml) },
    { ".tex", CT(application_x_tex) },
    { ".texi", CT(application_x_texinfo) },
    { ".texinfo", CT(application_x_texinfo) },
    { ".tgz", CT(application_x_compressed) },
    { ".thmx", CT(application_vnd_ms_officetheme) },
    { ".thn", CT(application_octet_stream) },
    { ".tif", CT(image_tiff) },
    { ".tiff", CT(image_tiff) },
    { ".tlh", CT(text_plain) },
    { ".tli", CT(text_plain) },
    { ".toc", CT(application_octet_stream) },
    { ".tr", CT(application_x_troff) },
    { ".trm", CT(application_x_msterminal) },
    { ".trx", CT(application_xml) },
    { ".ts", CT(video_vnd_dlna_mpeg_tts) },
    { ".tsv", CT(text_tab_separated_values) },
    { ".ttf", CT(application_octet_stream) },
    { ".tts", CT(video_vnd_dlna_mpeg_tts) },
    { ".txt", CT(text_plain) },
    { ".u32", CT(application_octet_stream) },
    { ".uls", CT(text_iuls) },
    { ".user", CT(text_plain) },
    { ".ustar", CT(application_x_ustar) },
    { ".vb", CT(text_plain) },
    { ".vbdproj", CT(text_plain) },
    { ".vbk", CT(video_mpeg) },
    { ".vbproj", CT(text_plain) },
    { ".vbs", CT(text_vbscript) },
    { ".vcf", CT(text_x_vcard) },
    { ".vcproj", CT(Application_xml) },
    { ".vcs", CT(text_plain) },
    { ".vcxproj", CT(Application_xml) },
    { ".vddproj", CT(text_plain) },
    { ".vdp", CT(text_plain) },
    { ".vdproj", CT(text_plain) },
    { ".vdx", CT(application_vnd_ms_visio_viewer) },
    { ".vml", CT(text_xml) },
    { ".vscontent", CT(application_xml) },
    { ".vsct", CT(text_xml) },
    { ".vsd", CT(application_vnd_visio) },
    { ".vsi", CT(application_ms_vsi) },
    { ".vsix", CT(application_vsix) },
    { ".vsixlangpack", CT(text_xml) },
    { ".vsixmanifest", CT(text_xml) },
    { ".vsmdi", CT(application_xml) },
    { ".vspscc", CT(text_plain) },
    { ".vss", CT(application_vnd_visio) },
    { ".vsscc", CT(text_plain) },
    { ".vssettings", CT(text_xml) },
    { ".vssscc", CT(text_plain) },
    { ".vst", CT(application_vnd_visio) },
    { ".vstemplate", CT(text_xml) },
    { ".vsto", CT(application_x_ms_vsto) },
    { ".vsw", CT(application_vnd_visio) },
    { ".vsx", CT(application_vnd_visio) },
    { ".vtx", CT(application_vnd_visio) },
    { ".wav", CT(audio_wav) },
    { ".wave", CT(audio_wav) },
    { ".wax", CT(audio_x_ms_wax) },
    { ".wbk", CT(application_msword) },
    { ".wbmp", CT(image_vnd_wap_wbmp) },
    { ".wcm", CT(application_vnd_ms_works) },
    { ".wdb", CT(application_vnd_ms_works) },
    { ".wdp", CT(image_vnd_ms_photo) },
    { ".webarchive", CT(application_x_safari_webarchive) },
    { ".webtest", CT(application_xml) },
    { ".wiq", CT(application_xml) },
    { ".wiz", CT(application_msword) },
    { ".wks", CT(application_vnd_ms_works) },
    { ".wlmp", CT(application_wlmoviemaker) },
    { ".wlpginstall", CT(application_x_wlpg_detect) },
    { ".wlpginstall3", CT(application_x_wlpg3_detect) },
    { ".wm", CT(video_x_ms_wm) },
    { ".wma", CT(audio_x_ms_wma) },
    { ".wmd", CT(application_x_ms_wmd) },
    { ".wmf", CT(application_x_msmetafile) },
    { ".wml", CT(text_vnd_wap_wml) },
    { ".wmlc", CT(application_vnd_wap_wmlc) },
    { ".wmls", CT(text_vnd_wap_wmlscript) },
    { ".wmlsc", CT(application_vnd_wap_wmlscriptc) },
    { ".wmp", CT(video_x_ms_wmp) },
    { ".wmv", CT(video_x_ms_wmv) },
    { ".wmx", CT(video_x_ms_wmx) },
    { ".wmz", CT(application_x_ms_wmz) },
    { ".wpl", CT(application_vnd_ms_wpl) },
    { ".wps", CT(application_vnd_ms_works) },
    { ".wri", CT(application_x_mswrite) },
    { ".wrl", CT(x_world_x_vrml) },
    { ".wrz", CT(x_world_x_vrml) },
    { ".wsc", CT(text_scriptlet) },
    { ".wsdl", CT(text_xml) },
    { ".wvx", CT(video_x_ms_wvx) },
    { ".x", CT(application_directx) },
    { ".xaf", CT(x_world_x_vrml) },
    { ".xaml", CT(application_xaml_xml) },
    { ".xap", CT(application_x_silverlight_app) },
    { ".xbap", CT(application_x_ms_xbap) },
    { ".xbm", CT(image_x_xbitmap) },
    { ".xdr", CT(text_plain) },
    { ".xht", CT(application_xhtml_xml) },
    { ".xhtml", CT(application_xhtml_xml) },
    { ".xla", CT(application_vnd_ms_excel) },
    { ".xlam", CT(application_vnd_ms_excel_addin_macroEnabled_12) },
    { ".xlc", CT(application_vnd_ms_excel) },
    { ".xld", CT(application_vnd_ms_excel) },
    { ".xlk", CT(application_vnd_ms_excel) },
    { ".xll", CT(application_vnd_ms_excel) },
    { ".xlm", CT(application_vnd_ms_excel) },
    { ".xls", CT(application_vnd_ms_excel) },
    { ".xlsb", CT(application_vnd_ms_excel_sheet_binary_macroEnabled_12) },
    { ".xlsm", CT(application_vnd_ms_excel_sheet_macroEnabled_12) },
    { ".xlsx", CT(application_vnd_openxmlformats_officedocument_spreadsheetml_sheet) },
    { ".xlt", CT(application_vnd_ms_excel) },
    { ".xltm", CT(application_vnd_ms_excel_template_macroEnabled_12) },
    { ".xltx", CT(application_vnd_openxmlformats_officedocument_spreadsheetml_template) },
    { ".xlw", CT(application_vnd_ms_excel) },
    { ".xml", CT(text_xml) },
    { ".xmta", CT(application_xml) },
    { ".xof", CT(x_world_x_vrml) },
    { ".xoml", CT(text_plain) },
    { ".xpm", CT(image_x_xpixmap) },
    { ".xps", CT(application_vnd_ms_xpsdocument) },
    { ".xrm-ms", CT(text_xml) },
    { ".xsc", CT(application_xml) },
    { ".xsd", CT(text_xml) },
    { ".xsf", CT(text_xml) },
    { ".xsl", CT(text_xml) },
    { ".xslt", CT(text_xml) },
    { ".xsn", CT(application_octet_stream) },
    { ".xss", CT(application_xml) },
    { ".xtp", CT(application_octet_stream) },
    { ".xwd", CT(image_x_xwindowdump) },
    { ".z", CT(application_x_compress) },
    { ".zip", CT(application_x_zip_compressed) },
};

// 供 bsearch 使用：key 为裸扩展名字符串，elem 为 contenttype_ctx
static int32_t _contenttype_cmp(const void *key, const void *elem) {
    return memcasecmp(key, ((const contenttype_ctx *)elem)->extension, strlen((const char *)key) + 1);// 连 '\0' 一起比，排序同 strcasecmp
}
const char *contenttype(const char *extension) {
    const contenttype_ctx *found = bsearch(extension, _typegreg,
        ARRAY_SIZE(_typegreg), sizeof(_typegreg[0]), _contenttype_cmp);
    return found ? (const char *)&_ct_types + found->type : "application/X-other-1";
}
