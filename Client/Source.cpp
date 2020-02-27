#include "pch.h"
#include "generated/chat.grpc.pb.h"

int main(int argc, char* argv[])
{
	using namespace grpc;
	using namespace Chat;

	auto channel = CreateChannel("localhost:50051", InsecureChannelCredentials());
	
	ClientContext ctx;
	auto client = ChatService::NewStub(channel);

	auto stream = client->Chat(&ctx);

	std::thread thread([&]
	{
		ChatMessage msg;
		while (stream->Read(&msg))
			std::cout << msg.value() << "\n";
	});

	ChatMessage msg;
	while (true)
	{
		std::cin >> *msg.mutable_value();
		if (msg.value() == "$quit")
			break;
		stream->Write(msg);
	}
	stream->Finish();
	thread.join();
	return 0;
}