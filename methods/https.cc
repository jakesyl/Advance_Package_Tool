//-*- mode: cpp; mode: fold -*-
// Description								/*{{{*/
// $Id: http.cc,v 1.59 2004/05/08 19:42:35 mdz Exp $
/* ######################################################################

   HTTPS Acquire Method - This is the HTTPS aquire method for APT.
   
   It uses libcurl

   ##################################################################### */
									/*}}}*/
// Include Files							/*{{{*/
#include <config.h>

#include <apt-pkg/fileutl.h>
#include <apt-pkg/acquire-method.h>
#include <apt-pkg/error.h>
#include <apt-pkg/hashes.h>
#include <apt-pkg/netrc.h>
#include <apt-pkg/configuration.h>

#include <sys/stat.h>
#include <sys/time.h>
#include <utime.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <iostream>
#include <sstream>

#include "config.h"
#include "https.h"
#include <apti18n.h>
									/*}}}*/
using namespace std;

size_t
HttpsMethod::parse_header(void *buffer, size_t size, size_t nmemb, void *userp)
{
   size_t len = size * nmemb;
   HttpsMethod *me = (HttpsMethod *)userp;
   std::string line((char*) buffer, len);
   for (--len; len > 0; --len)
      if (isspace(line[len]) == 0)
      {
	 ++len;
	 break;
      }
   line.erase(len);

   if (line.empty() == true)
   {
      if (me->Server->Result != 416 && me->Server->StartPos != 0)
	 ;
      else if (me->Server->Result == 416 && me->Server->Size == me->File->FileSize())
      {
         me->Server->Result = 200;
	 me->Server->StartPos = me->Server->Size;
      }
      else
	 me->Server->StartPos = 0;

      me->File->Truncate(me->Server->StartPos);
      me->File->Seek(me->Server->StartPos);
   }
   else if (me->Server->HeaderLine(line) == false)
      return 0;

   return size*nmemb;
}

size_t 
HttpsMethod::write_data(void *buffer, size_t size, size_t nmemb, void *userp)
{
   HttpsMethod *me = (HttpsMethod *)userp;

   if(me->File->Write(buffer, size*nmemb) != true)
      return false;

   return size*nmemb;
}

int 
HttpsMethod::progress_callback(void *clientp, double dltotal, double dlnow, 
			      double ultotal, double ulnow)
{
   HttpsMethod *me = (HttpsMethod *)clientp;
   if(dltotal > 0 && me->Res.Size == 0) {
      me->Res.Size = (unsigned long long)dltotal;
      me->URIStart(me->Res);
   }
   return 0;
}

// HttpsServerState::HttpsServerState - Constructor			/*{{{*/
HttpsServerState::HttpsServerState(URI Srv,HttpsMethod *Owner) : ServerState(Srv, NULL)
{
   TimeOut = _config->FindI("Acquire::https::Timeout",TimeOut);
   Reset();
}
									/*}}}*/

void HttpsMethod::SetupProxy()  					/*{{{*/
{
   URI ServerName = Queue->Uri;

   // Curl should never read proxy settings from the environment, as
   // we determine which proxy to use.  Do this for consistency among
   // methods and prevent an environment variable overriding a
   // no-proxy ("DIRECT") setting in apt.conf.
   curl_easy_setopt(curl, CURLOPT_PROXY, "");

   // Determine the proxy setting - try https first, fallback to http and use env at last
   string UseProxy = _config->Find("Acquire::https::Proxy::" + ServerName.Host,
				   _config->Find("Acquire::http::Proxy::" + ServerName.Host).c_str());

   if (UseProxy.empty() == true)
      UseProxy = _config->Find("Acquire::https::Proxy", _config->Find("Acquire::http::Proxy").c_str());

   // User want to use NO proxy, so nothing to setup
   if (UseProxy == "DIRECT")
      return;

   if (UseProxy.empty() == false) 
   {
      // Parse no_proxy, a comma (,) separated list of domains we don't want to use
      // a proxy for so we stop right here if it is in the list
      if (getenv("no_proxy") != 0 && CheckDomainList(ServerName.Host,getenv("no_proxy")) == true)
	 return;
   } else {
      const char* result = getenv("https_proxy");
      // FIXME: Fall back to http_proxy is to remain compatible with
      // existing setups and behaviour of apt.conf.  This should be
      // deprecated in the future (including apt.conf).  Most other
      // programs do not fall back to http proxy settings and neither
      // should Apt.
      if (result == NULL)
         result = getenv("http_proxy");
      UseProxy = result == NULL ? "" : result;
   }

   // Determine what host and port to use based on the proxy settings
   if (UseProxy.empty() == false) 
   {
      Proxy = UseProxy;
      if (Proxy.Port != 1)
	 curl_easy_setopt(curl, CURLOPT_PROXYPORT, Proxy.Port);
      curl_easy_setopt(curl, CURLOPT_PROXY, Proxy.Host.c_str());
      if (Proxy.User.empty() == false || Proxy.Password.empty() == false)
      {
         curl_easy_setopt(curl, CURLOPT_PROXYUSERNAME, Proxy.User.c_str());
         curl_easy_setopt(curl, CURLOPT_PROXYPASSWORD, Proxy.Password.c_str());
      }
   }
}									/*}}}*/
// HttpsMethod::Fetch - Fetch an item					/*{{{*/
// ---------------------------------------------------------------------
/* This adds an item to the pipeline. We keep the pipeline at a fixed
   depth. */
bool HttpsMethod::Fetch(FetchItem *Itm)
{
   struct stat SBuf;
   struct curl_slist *headers=NULL;  
   char curl_errorstr[CURL_ERROR_SIZE];
   URI Uri = Itm->Uri;
   string remotehost = Uri.Host;

   // TODO:
   //       - http::Pipeline-Depth
   //       - error checking/reporting
   //       - more debug options? (CURLOPT_DEBUGFUNCTION?)

   curl_easy_reset(curl);
   SetupProxy();

   maybe_add_auth (Uri, _config->FindFile("Dir::Etc::netrc"));

   // callbacks
   curl_easy_setopt(curl, CURLOPT_URL, static_cast<string>(Uri).c_str());
   curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, parse_header);
   curl_easy_setopt(curl, CURLOPT_WRITEHEADER, this);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
   curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, progress_callback);
   curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, this);
   curl_easy_setopt(curl, CURLOPT_NOPROGRESS, false);
   curl_easy_setopt(curl, CURLOPT_FILETIME, true);

   // SSL parameters are set by default to the common (non mirror-specific) value
   // if available (or a default one) and gets overload by mirror-specific ones.

   // File containing the list of trusted CA.
   string cainfo = _config->Find("Acquire::https::CaInfo","");
   string knob = "Acquire::https::"+remotehost+"::CaInfo";
   cainfo = _config->Find(knob.c_str(),cainfo.c_str());
   if(cainfo.empty() == false)
      curl_easy_setopt(curl, CURLOPT_CAINFO,cainfo.c_str());

   // Check server certificate against previous CA list ...
   bool peer_verify = _config->FindB("Acquire::https::Verify-Peer",true);
   knob = "Acquire::https::" + remotehost + "::Verify-Peer";
   peer_verify = _config->FindB(knob.c_str(), peer_verify);
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, peer_verify);

   // ... and hostname against cert CN or subjectAltName
   bool verify = _config->FindB("Acquire::https::Verify-Host",true);
   knob = "Acquire::https::"+remotehost+"::Verify-Host";
   verify = _config->FindB(knob.c_str(),verify);
   int const default_verify = (verify == true) ? 2 : 0;
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, default_verify);

   // Also enforce issuer of server certificate using its cert
   string issuercert = _config->Find("Acquire::https::IssuerCert","");
   knob = "Acquire::https::"+remotehost+"::IssuerCert";
   issuercert = _config->Find(knob.c_str(),issuercert.c_str());
   if(issuercert.empty() == false)
      curl_easy_setopt(curl, CURLOPT_ISSUERCERT,issuercert.c_str());

   // For client authentication, certificate file ...
   string pem = _config->Find("Acquire::https::SslCert","");
   knob = "Acquire::https::"+remotehost+"::SslCert";
   pem = _config->Find(knob.c_str(),pem.c_str());
   if(pem.empty() == false)
      curl_easy_setopt(curl, CURLOPT_SSLCERT, pem.c_str());

   // ... and associated key.
   string key = _config->Find("Acquire::https::SslKey","");
   knob = "Acquire::https::"+remotehost+"::SslKey";
   key = _config->Find(knob.c_str(),key.c_str());
   if(key.empty() == false)
      curl_easy_setopt(curl, CURLOPT_SSLKEY, key.c_str());

   // Allow forcing SSL version to SSLv3 or TLSv1 (SSLv2 is not
   // supported by GnuTLS).
   long final_version = CURL_SSLVERSION_DEFAULT;
   string sslversion = _config->Find("Acquire::https::SslForceVersion","");
   knob = "Acquire::https::"+remotehost+"::SslForceVersion";
   sslversion = _config->Find(knob.c_str(),sslversion.c_str());
   if(sslversion == "TLSv1")
     final_version = CURL_SSLVERSION_TLSv1;
   else if(sslversion == "SSLv3")
     final_version = CURL_SSLVERSION_SSLv3;
   curl_easy_setopt(curl, CURLOPT_SSLVERSION, final_version);

   // CRL file
   string crlfile = _config->Find("Acquire::https::CrlFile","");
   knob = "Acquire::https::"+remotehost+"::CrlFile";
   crlfile = _config->Find(knob.c_str(),crlfile.c_str());
   if(crlfile.empty() == false)
      curl_easy_setopt(curl, CURLOPT_CRLFILE, crlfile.c_str());

   // cache-control
   if(_config->FindB("Acquire::https::No-Cache",
	_config->FindB("Acquire::http::No-Cache",false)) == false)
   {
      // cache enabled
      if (_config->FindB("Acquire::https::No-Store",
		_config->FindB("Acquire::http::No-Store",false)) == true)
	 headers = curl_slist_append(headers,"Cache-Control: no-store");
      stringstream ss;
      ioprintf(ss, "Cache-Control: max-age=%u", _config->FindI("Acquire::https::Max-Age",
		_config->FindI("Acquire::http::Max-Age",0)));
      headers = curl_slist_append(headers, ss.str().c_str());
   } else {
      // cache disabled by user
      headers = curl_slist_append(headers, "Cache-Control: no-cache");
      headers = curl_slist_append(headers, "Pragma: no-cache");
   }
   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

   // speed limit
   int const dlLimit = _config->FindI("Acquire::https::Dl-Limit",
		_config->FindI("Acquire::http::Dl-Limit",0))*1024;
   if (dlLimit > 0)
      curl_easy_setopt(curl, CURLOPT_MAX_RECV_SPEED_LARGE, dlLimit);

   // set header
   curl_easy_setopt(curl, CURLOPT_USERAGENT,
	_config->Find("Acquire::https::User-Agent",
		_config->Find("Acquire::http::User-Agent",
			"Debian APT-CURL/1.0 (" PACKAGE_VERSION ")").c_str()).c_str());

   // set timeout
   int const timeout = _config->FindI("Acquire::https::Timeout",
		_config->FindI("Acquire::http::Timeout",120));
   curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout);
   //set really low lowspeed timeout (see #497983)
   curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, DL_MIN_SPEED);
   curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, timeout);

   // set redirect options and default to 10 redirects
   bool const AllowRedirect = _config->FindB("Acquire::https::AllowRedirect",
	_config->FindB("Acquire::http::AllowRedirect",true));
   curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, AllowRedirect);
   curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10);

   // debug
   if(_config->FindB("Debug::Acquire::https", false))
      curl_easy_setopt(curl, CURLOPT_VERBOSE, true);

   // error handling
   curl_errorstr[0] = '\0';
   curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_errorstr);

   // If we ask for uncompressed files servers might respond with content-
   // negotation which lets us end up with compressed files we do not support,
   // see 657029, 657560 and co, so if we have no extension on the request
   // ask for text only. As a sidenote: If there is nothing to negotate servers
   // seem to be nice and ignore it.
   if (_config->FindB("Acquire::https::SendAccept", _config->FindB("Acquire::http::SendAccept", true)) == true)
   {
      size_t const filepos = Itm->Uri.find_last_of('/');
      string const file = Itm->Uri.substr(filepos + 1);
      if (flExtension(file) == file)
	 headers = curl_slist_append(headers, "Accept: text/*");
   }

   // if we have the file send an if-range query with a range header
   if (stat(Itm->DestFile.c_str(),&SBuf) >= 0 && SBuf.st_size > 0)
   {
      char Buf[1000];
      sprintf(Buf, "Range: bytes=%li-", (long) SBuf.st_size);
      headers = curl_slist_append(headers, Buf);
      sprintf(Buf, "If-Range: %s", TimeRFC1123(SBuf.st_mtime).c_str());
      headers = curl_slist_append(headers, Buf);
   }
   else if(Itm->LastModified > 0)
   {
      curl_easy_setopt(curl, CURLOPT_TIMECONDITION, CURL_TIMECOND_IFMODSINCE);
      curl_easy_setopt(curl, CURLOPT_TIMEVALUE, Itm->LastModified);
   }

   // go for it - if the file exists, append on it
   File = new FileFd(Itm->DestFile, FileFd::WriteAny);
   Server = new HttpsServerState(Itm->Uri, this);

   // keep apt updated
   Res.Filename = Itm->DestFile;

   // get it!
   CURLcode success = curl_easy_perform(curl);

   // If the server returns 200 OK but the If-Modified-Since condition is not
   // met, CURLINFO_CONDITION_UNMET will be set to 1
   long curl_condition_unmet = 0;
   curl_easy_getinfo(curl, CURLINFO_CONDITION_UNMET, &curl_condition_unmet);

   File->Close();
   curl_slist_free_all(headers);

   // cleanup
   if (success != 0)
   {
      _error->Error("%s", curl_errorstr);
      unlink(File->Name().c_str());
      return false;
   }

   // server says file not modified
   if (Server->Result == 304 || curl_condition_unmet == 1)
   {
      unlink(File->Name().c_str());
      Res.IMSHit = true;
      Res.LastModified = Itm->LastModified;
      Res.Size = 0;
      URIDone(Res);
      return true;
   }
   Res.IMSHit = false;

   if (Server->Result != 200 && // OK
	 Server->Result != 206 && // Partial
	 Server->Result != 416) // invalid Range
   {
      char err[255];
      snprintf(err, sizeof(err) - 1, "HttpError%i", Server->Result);
      SetFailReason(err);
      _error->Error("%s", err);
      // unlink, no need keep 401/404 page content in partial/
      unlink(File->Name().c_str());
      return false;
   }

   struct stat resultStat;
   if (unlikely(stat(File->Name().c_str(), &resultStat) != 0))
   {
      _error->Errno("stat", "Unable to access file %s", File->Name().c_str());
      return false;
   }
   Res.Size = resultStat.st_size;

   // invalid range-request
   if (Server->Result == 416)
   {
      unlink(File->Name().c_str());
      Res.Size = 0;
      delete File;
      Redirect(Itm->Uri);
      return true;
   }

   // Timestamp
   curl_easy_getinfo(curl, CURLINFO_FILETIME, &Res.LastModified);
   if (Res.LastModified != -1)
   {
      struct utimbuf UBuf;
      UBuf.actime = Res.LastModified;
      UBuf.modtime = Res.LastModified;
      utime(File->Name().c_str(),&UBuf);
   }
   else
      Res.LastModified = resultStat.st_mtime;

   // take hashes
   Hashes Hash;
   FileFd Fd(Res.Filename, FileFd::ReadOnly);
   Hash.AddFD(Fd);
   Res.TakeHashes(Hash);

   // keep apt updated
   URIDone(Res);

   // cleanup
   Res.Size = 0;
   delete File;

   return true;
};

int main()
{
   setlocale(LC_ALL, "");

   HttpsMethod Mth;
   curl_global_init(CURL_GLOBAL_SSL) ;

   return Mth.Run();
}

