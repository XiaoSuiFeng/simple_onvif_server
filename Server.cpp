#include "Server.h"
#include "../onvif_services/device_service.h"
#include "../onvif_services/media_service.h"
#include "../onvif_services/media2_service.h"
#include "../onvif_services/event_service.h"
#include "../onvif_services/discovery_service.h"
#include "utility/XmlParser.h"

#include "utility/AuthHelper.h"

#include "Simple-Web-Server\server_http.hpp"

#include <string>

#include <boost\property_tree\json_parser.hpp>

static const std::string COMMON_CONFIGS_NAME = "common.config";

namespace osrv
{
	
	AUTH_SCHEME str_to_auth(const std::string& /*scheme*/);

	Server::Server(std::string configs_dir, ILogger& log)
		:logger_(log)
		,http_server_instance_(new HttpServer)
	{
		http_server_instance_->config.port = MASTER_PORT;
	
		http_server_instance_->default_resource["GET"] = [](std::shared_ptr<HttpServer::Response> response,
			std::shared_ptr<HttpServer::Request> request)
		{
			response->write(SimpleWeb::StatusCode::client_error_bad_request, "Could not open path " + request->path);
		};
		
		http_server_instance_->default_resource["POST"] = [this](std::shared_ptr<HttpServer::Response> response,
			std::shared_ptr<HttpServer::Request> request)
		{
			logger_.Warn("The server could not handle a request:" + request->method + " " + request->path);
			response->write(SimpleWeb::StatusCode::client_error_bad_request, "Bad request");
		};


		configs_dir += "/";
		// TODO: as now here we read configs, we can refactor @read_server_configs function and pass ptree 
		std::ifstream configs_file(configs_dir + COMMON_CONFIGS_NAME);
		if (configs_file.is_open())
		{
			namespace pt = boost::property_tree;
			pt::ptree configs_tree;
			pt::read_json(configs_file, configs_tree);
			auto log_lvl = configs_tree.get<std::string>("loggingLevel", "");
			if (!log_lvl.empty())
				logger_.SetLogLevel(ILogger::to_lvl(log_lvl));
		}

		server_configs_ = read_server_configs(configs_dir + COMMON_CONFIGS_NAME);
		server_configs_.digest_session_ = std::make_shared<utility::digest::DigestSessionImpl>();
		//TODO: here is the same list is copied into digest_session, although it's already stored in server_configs
		server_configs_.digest_session_->set_users_list(server_configs_.system_users_);

		device::init_service(*http_server_instance_, server_configs_, configs_dir, log);
		media::init_service(*http_server_instance_, server_configs_, configs_dir, log);
		media2::init_service(*http_server_instance_, server_configs_, configs_dir, log);
		// the event service is not completed yet, comment for master branch for now
		// event::init_service(*http_server_instance_, server_configs_, configs_dir, log);
		discovery::init_service(configs_dir, log);

		rtspServer_ = new rtsp::Server(&log, server_configs_.ipv4_address_.c_str(), server_configs_.rtsp_port_.c_str());
	}

	Server::~Server()
	{
		discovery::stop();
		delete rtspServer_;
	}

void Server::run()
{
	using namespace std;

	// Start server and receive assigned port when server is listening for requests
	promise<unsigned short> server_port;
	thread server_thread([this, &server_port]() {
		// Start server
		http_server_instance_->start([&server_port](unsigned short port) {
				server_port.set_value(port);
			});
		});

	rtspServer_->run();
	try
	{
		discovery::start();
	}
	catch (const std::exception& e)
	{
		std::string what(e.what());
		logger_.Error("Can't start Discovery Service: " + what);
	}

	std::string msg("Server is successfully started on port: ");
	msg += std::to_string(server_port.get_future().get());
	logger_.Info(msg);

	server_thread.join();
}

ServerConfigs read_server_configs(const std::string& config_path)
{

	std::ifstream configs_file(config_path);
	if (!configs_file.is_open())
		throw std::runtime_error("Could not read a config file");

	namespace pt = boost::property_tree;
	pt::ptree configs_tree;
	pt::read_json(configs_file, configs_tree);
	
	ServerConfigs read_configs;

	read_configs.ipv4_address_ = configs_tree.get<std::string>("addresses.ipv4");
	read_configs.http_port_ = configs_tree.get<std::string>("addresses.http_port");
	read_configs.rtsp_port_ = configs_tree.get<std::string>("addresses.rtsp_port");

	auto auth_scheme = configs_tree.get<std::string>("authentication");
	read_configs.auth_scheme_ = str_to_auth(auth_scheme);

	auto users_node = configs_tree.get_child("users");
	if (users_node.empty())
		throw std::runtime_error("Could not read Users list");

	for (auto user : users_node)
	{
		read_configs.system_users_.emplace_back(
			osrv::auth::UserAccount{
				user.second.get<std::string>(auth::UserAccount::LOGIN),
				user.second.get<std::string>(auth::UserAccount::PASS),
				osrv::auth::str_to_usertype(user.second.get<std::string>(auth::UserAccount::TYPE))
			});
	}

	return read_configs;
}

AUTH_SCHEME str_to_auth(const std::string& scheme)
{
	if (scheme == "digest/ws-security")
		return AUTH_SCHEME::DIGEST_WSS;
	
	if (scheme == "digest")
		return AUTH_SCHEME::DIGEST;
	
	if (scheme == "ws-security")
		return AUTH_SCHEME::WSS;

	return AUTH_SCHEME::NONE;
}

}
