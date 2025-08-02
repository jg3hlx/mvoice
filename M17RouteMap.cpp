/*
 *   Copyright (c) 2020-2021 by Thomas A. Early N7TAE
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <fstream>
#include <regex>

#include "M17RouteMap.h"
#include "Utilities.h"

using json = nlohmann::json;

// callback function writes data to a std::ostream
static size_t data_write(void* buf, size_t size, size_t nmemb, void* userp)
{
	if(userp)
	{
		std::ostream& os = *static_cast<std::ostream*>(userp);
		std::streamsize len = size * nmemb;
		if(os.write(static_cast<char*>(buf), len))
			return len;
	}

	return 0;
}

// timeout is in seconds
static CURLcode curl_read(const std::string& url, std::ostream& os, long timeout = 30)
{
	CURLcode code(CURLE_FAILED_INIT);
	CURL* curl = curl_easy_init();

	if(curl)
	{
		if(CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &data_write))
		&& CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L))
		&& CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L))
		&& CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_FILE, &os))
		&& CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout))
		&& CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_USERAGENT, "libcurl-agent/1.0"))
		&& CURLE_OK == (code = curl_easy_setopt(curl, CURLOPT_URL, url.c_str())))
		{
			code = curl_easy_perform(curl);
		}
		curl_easy_cleanup(curl);
	}
	return code;
}

// returns true if there was a problem
static bool ReadM17Json(const std::string &url, std::stringstream &ss)
{
	bool rval = true;
	curl_global_init(CURL_GLOBAL_ALL);

	if(CURLE_OK == curl_read(url, ss))
		rval = false;

	curl_global_cleanup();
	return rval;
}

CM17RouteMap::CM17RouteMap() {}

CM17RouteMap::~CM17RouteMap()
{
	std::lock_guard<std::mutex> lck(mux);
	baseMap.clear();
}

const std::shared_ptr<SHost> CM17RouteMap::Find(const std::string &cs) const
{
	std::shared_ptr<SHost> rval = nullptr;
	std::string base;
	auto pos = cs.find_first_of(" /.");
	if (pos < 3)
		return rval;
	base.assign(cs, 0, pos);
	std::lock_guard<std::mutex> lck(mux);
	auto bit = baseMap.find(cs);
	if (bit != baseMap.end())
		rval = bit->second;
	return rval;
}

void CM17RouteMap::Update(EFrom from, const std::string &cs, bool islegacy, const std::string &domainname, const std::string &ip4addr, const std::string &ip6addr, const std::string &modules, const std::string &specialmodules, const uint16_t port, const std::string &url)
{
	std::string base;
	auto pos = cs.find_first_of(" /.");
	if (pos < 3)
		return;
	base.assign(cs, 0, pos);
	auto host = Find(base);
	if (! host)
		host = std::make_shared<SHost>();
	host->from = from;
	host->cs.assign(base);
	host->is_legacy = islegacy;
	if (! domainname.empty())
		host->dn.assign(domainname);
	if (! ip4addr.empty())
		host->ip4addr.assign(ip4addr);
	if (! ip6addr.empty())
		host->ip6addr.assign(ip6addr);
	if (! modules.empty())
		host->mods.assign(modules);
	if (! specialmodules.empty())
		host->smods.assign(specialmodules);
	if (! url.empty())
		host->url.assign(url);
	host->port = port;

	std::lock_guard<std::mutex> lck(mux);
	if (host->dn.size() || host->ip4addr.size() || host->ip6addr.size() || host->url.size() || host->mods.size() || host->smods.size())
		host->updated = true;
	baseMap[base] = host;
	//std::cout << "updating " << host->cs << ": dn='" << host->dn << "' ipv4='" << host->ip4addr << "' ipv6='" << host->ip6addr << "' url='" << host->url << "' mods='" << host->mods << "' smods='" << host->smods << "' port=" << host->port << std::endl;

}

void CM17RouteMap::ReadAll()
{
	mux.lock();
	baseMap.clear();
	mux.unlock();
	ReadJson();
	Read();
}

#define GET_STRING(a) ((a).is_string() ? a : "")

void CM17RouteMap::ReadJson()
{
	// downlaod and parse the mrefd and urf json file
	std::stringstream ss;
	if (ReadM17Json("https://hostfiles.refcheck.radio/M17Hosts.json", ss))
	{
		std::cerr << "ERROR curling M17 reflectors from hostfiles.refcheck.radio/M17Hosts.json" << std::endl;
	}
	else
	{
		json mref;

		try {
			mref = json::parse(ss.str());
		}
		catch (const std::exception &e) {
			std::cerr << e.what() << std::endl;
			return;
		}
		
		if (mref.contains("reflectors"))
		{
			for (auto &ref : mref["reflectors"])
			{
				const std::string cs(GET_STRING(ref["designator"]));
				if (0 == cs.substr(0,4).compare("M17-"))
				{
					const std::string dn(GET_STRING(ref["dns"]));
					const std::string ipv4(GET_STRING(ref["ipv4"]));
					if (0==ipv4.compare("127.0.0.1") or 0==ipv4.compare("0.0.0.0"))
						continue;
					const std::string ipv6(GET_STRING(ref["ipv6"]));
					if (0==ipv6.compare("::") or 0==ipv6.compare("::1"))
						continue;
					std::string mods(""), emods("");
					if (ref.contains("modules"))
					{
						for (const auto &item : ref["modules"])
							mods.append(item);
					}
					if (ref.contains("encrypted"))
					{
						for (const auto &item : ref["encrypted"])
							emods.append(item);
					}
					uint16_t port;
					if (ref.contains("port") and ref["port"].is_number_unsigned())
						port = ref["port"].get<uint16_t>();
					else
						continue;
					Update(EFrom::json, cs, true, dn, ipv4, ipv6, mods, emods, port, GET_STRING(ref["url"]));
				}
				else if (0 == cs.substr(0,3).compare("URF"))
				{
					const std::string dn(GET_STRING(ref["dns"]));
					const std::string ipv4(GET_STRING(ref["ipv4"]));
					if (0==ipv4.compare("127.0.0.1") or 0==ipv4.compare("0.0.0.0"))
						continue;
					const std::string ipv6(GET_STRING(ref["ipv6"]));
					if (0==ipv6.compare("::") or 0==ipv6.compare("::1"))
						continue;
					std::string mods(""), smods("");
					uint16_t port = 17000u;
					if (ref.contains("modules"))
					{
						for (auto &mod : ref["modules"])
						{
							auto m = mod["module"].get<std::string>();
							const std::string mode(GET_STRING(mod["mode"]));
							if (0==mode.compare("All") or 0==mode.compare("M17"))
							{
								mods.append(m);
								if (mod["transcode"].is_boolean())
								{
									if (mod["transcode"].get<bool>())
										smods.append(m);
								}
								if (0 == mode.compare("M17"))
								{
									if (mod["port"].is_number_unsigned())
										port = mod["port"].get<uint16_t>();
								}
							}
						}
					}
					Update(EFrom::json, cs, true, dn, ipv4, ipv6, mods, smods, port, GET_STRING(ref["url"]));
				}
			}
		}
		else
		{
			std::cerr << "ERROR: hostfiles.refcheck.radio didn't define any M17 reflectors" << std::endl;
		}
	}
}

static const char *FILENAME = "/MyM17Hosts.txt";

void CM17RouteMap::Read()
{
	std::string path(CFGDIR);
	path.append(FILENAME);
	std::ifstream file(path, std::ifstream::in);
	if (file.is_open()) {
		std::string line;
		while (getline(file, line)) {
			trim(line);
			if (0==line.size() || '#'==line[0]) continue;
			std::vector<std::string> elem;
			split(line, ';', elem);
			switch (elem.size())
			{
			case 8:
				Update(EFrom::user, elem[0], true, elem[1], elem[2], elem[3], elem[4], elem[5], std::stoul(elem[6]), elem[7]);
				break;
			case 9:
				Update(EFrom::user, elem[0], elem[1].compare("false")?true:false, elem[2], elem[3], elem[4], elem[5], elem[6], std::stoul(elem[7]), elem[8]);
				break;
			default:
				break;
			}
		}
		file.close();
	}
}

void CM17RouteMap::Save() const
{
	std::string path(CFGDIR);
	path.append(FILENAME);
	std::ofstream file(path.c_str(), std::ofstream::out | std::ofstream::trunc);
	file << "#Callsign;IsLegacy;DomainName;IPv4Address;IPv6Address;Modules;SpecialModules;Port;DashboardURL" << std::endl;
	if (file.is_open()) {
		mux.lock();
		for (const auto &pair : baseMap) {
			const auto host = pair.second;
			if (EFrom::user == host->from) {
				file << host->cs << ';' << (host->is_legacy?"true":"false") << host->dn << ';' << host->ip4addr << ';' << host->ip6addr << ';' << host->mods << ';' << host->smods << ';' << host->port << ';' << host->url << std::endl;
			}
		}
		file.close();
		mux.unlock();
	}
}

const std::list<std::string> CM17RouteMap::GetKeys() const
{
	std::list<std::string> keys;
	mux.lock();
	for (const auto &pair : baseMap)
		keys.push_back(pair.first);
	mux.unlock();
	return keys;
}

unsigned CM17RouteMap::CountKeysThatBeginsWith(const std::string &str) const
{
	unsigned rv = 0;
	for (const auto item : baseMap)
	{
		if (0 == item.first.find(str))
		{
			rv++;
		}
	}
	return rv;
}

void CM17RouteMap::Erase(const std::string &cs)
{
	mux.lock();
	auto it = baseMap.find(cs);
	if (it != baseMap.end())
		baseMap.erase(it);
	mux.unlock();
}

size_t CM17RouteMap::Size() const
{
	return baseMap.size();
}
