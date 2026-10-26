#pragma once
// (A copy of ReSaverHelper\src\Protocol.h: keep the two the same.)
// ReSaverHelper <-> ReSaver++ protocol, over the local named pipe \\.\pipe\ReSaverHelper (owner-only access).
// One request per line, one reply line per request. Fields are separated by tabs; tab, newline and
// backslash inside a field are written as \t, \n and \\.
//
//   request: <id>\t<COMMAND>\t<arg>...          reply: <id>\tOK\t<field>...   or   <id>\tERR\t<message>
//
//   HELLO                                 -> OK  ReSaverHelper  <helper version>  <game version>  <SE|AE|VR>  <in game 0|1>
//   QUEST <formid hex>                    -> OK  <editor id>  <flags hex>  <current stage>  <running 0|1>  <completed 0|1>  <index:state,...>
//   SETSTAGE <formid hex> <stage> <notify> <message>   -> OK <1 set, 0 refused, ? still running>  <running 0|1>  <completed 0|1>  <current stage>
//   COMPLETE <formid hex> <notify> <message>           -> OK <1 if the quest is now completed, 0 if not, ? still running>
//                                         (CompleteAllObjectives + CompleteQuest: no stage script runs; version 3)
//   REFRESH <notify>                      -> OK  <objectives to refresh>        (runs in the background, one quest at a time;
//                                            ERR while a menu is open; pauses while one is opened - version 4)
//   PROGRESS                              -> OK  <done>  <total>  <running 0|1>
//   GETOPTION <name>                      -> OK  <value>
//   SETOPTION <name> <value>              -> OK                                 (saved to ReSaverHelper.ini)
//   NOTIFY <notify> <message>             -> OK
//   SNAPSHOT                              -> OK  <file written>  <records>      (version 5: see Transplant.h)
//   APPLY <record fields...>              -> OK  1 | OK 0 <why not> | OK ?     (one snapshot record, applied to the loaded game)
//   RESOLVE <plugin> <local id hex>       -> OK  <runtime form id hex>
// <notify>: hud (top-left notification), box (message box), none, or ini (the helper's own setting).
#include <string>
#include <vector>

namespace proto
{
	inline constexpr const wchar_t* kPipeName = L"\\\\.\\pipe\\ReSaverHelper";
	inline constexpr int            kHelperVersion = 10;

	inline std::string Escape(const std::string& a_in)
	{
		std::string o;
		for (char c : a_in) {
			switch (c) {
			case '\\': o += "\\\\"; break;
			case '\t': o += "\\t"; break;
			case '\n': o += "\\n"; break;
			case '\r': break;
			default: o += c;
			}
		}
		return o;
	}

	inline std::vector<std::string> Split(const std::string& a_line)
	{
		std::vector<std::string> out(1);
		for (std::size_t i = 0; i < a_line.size(); ++i) {
			const char c = a_line[i];
			if (c == '\t') {
				out.emplace_back();
			} else if (c == '\\' && i + 1 < a_line.size()) {
				const char n = a_line[++i];
				out.back() += n == 't' ? '\t' : n == 'n' ? '\n' : n;
			} else if (c != '\r') {
				out.back() += c;
			}
		}
		return out;
	}

	inline std::string Join(const std::vector<std::string>& a_fields)
	{
		std::string o;
		for (std::size_t i = 0; i < a_fields.size(); ++i) {
			o += (i ? "\t" : "") + Escape(a_fields[i]);
		}
		return o;
	}
}
