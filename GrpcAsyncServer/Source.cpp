#include "pch.h"
#include "GrpcAsync.h"
#include "generated/chat.grpc.pb.h"

namespace Chat
{
	using grpc::Status;
	using grpc::ServerContext;
	
	struct ChatAsyncService : ChatService::AsyncService
	{
		void Bind(GrpcAsync::ServiceBinder& binder)
		{
			binder.Bind(this, &ChatAsyncService::RequestMessage, &ChatAsyncService::Message);
			binder.Bind(this, &ChatAsyncService::RequestClientStreamingMessage, &ChatAsyncService::ClientStreamingMessage);
			binder.Bind(this, &ChatAsyncService::RequestServerStreamingMessage, &ChatAsyncService::ServerStreamingMessage);
			binder.Bind(this, &ChatAsyncService::RequestChat, &ChatAsyncService::Chat);
		}

		bool quit() const { return _quit; }
		
	private:
		bool _quit = false;
		std::list<grpc::ServerReaderWriterInterface<ChatMessage, ChatMessage>*> streams;

		void WriteToAll(const ChatMessage& msg, grpc::ServerReaderWriterInterface<ChatMessage, ChatMessage>* exclude) const
		{
			for (auto stream : streams) if (stream != exclude)
				stream->Write(msg);
			std::cout << msg.value() << "\n";
		}

		Status Message(ServerContext*, const ChatMessage* request, ChatMessage* response) override
		{
			if (request->value() == "$quit")
			{
				_quit = true;
				response->set_value("Server Closed.");
			}
			else *response = *request;
			return Status::OK;
		}
		
		Status ClientStreamingMessage(ServerContext*, grpc::ServerReaderInterface<ChatMessage>* reader, ChatMessage* response) /*override*/
		{
			std::ostringstream ss;
			ChatMessage msg;
			while (reader->Read(&msg))
				ss << msg.value() << std::endl;
			response->set_value(ss.str());
			return Status::OK;
		}
		
		Status ServerStreamingMessage(ServerContext*, const ChatMessage* request, grpc::ServerWriterInterface<ChatMessage>* writer) /*override*/
		{
			using namespace std::chrono_literals;
			for (int i = 0; i < 5; ++i)
			{
				std::this_thread::sleep_for(.2s); //This will indeed block the main thread. lets just pretend is a expensive calculation.
				writer->Write(*request);
			}
			return Status::OK;
		}
		
		Status Chat(ServerContext*, grpc::ServerReaderWriterInterface<ChatMessage, ChatMessage>* stream) /*override*/
		{
			using namespace std::string_literals;
			std::ostringstream ss;
			ss << stream << " Connected.";
			ChatMessage msg;
			msg.set_value(ss.str());
			WriteToAll(msg, stream);
			streams.emplace_back(stream);

			while (stream->Read(&msg))
			{
				if (msg.value() == "$throw")
					throw std::runtime_error("Throwed by user.");
				ss.clear();
				ss.str("");
				ss << stream << ": " << msg.value();
				msg.set_value(ss.str());
				WriteToAll(msg, stream);
			}
			
			streams.remove(stream);

			ss.clear();
			ss.str("");
			ss << stream << " Disconnected.";
			msg.set_value(ss.str());
			WriteToAll(msg, stream);
			
			return Status::OK;
		}
		
	};
	
}

int main(int argc, char* argv[])
{
	using Chat::ChatAsyncService;

	grpc::ServerBuilder builder;
	ChatAsyncService service;

	auto cq = builder
		.AddListeningPort("localhost:50051", grpc::InsecureServerCredentials())
		.RegisterService(&service)
		.AddCompletionQueue();

	auto server = builder.BuildAndStart();

	GrpcAsync::ServiceBinder binder(cq.get());

	binder.AddExceptionHandler(+[](std::exception& e)
	{
		std::cout << e.what() << "\n";
		return grpc::Status{ grpc::ABORTED, "exception", e.what() };
	});
	
	service.Bind(binder);
	
	void* tag;
	bool ok;
	do
	{
		//std::cout << "Server running...\r";
		if (cq->AsyncNext(&tag, &ok, std::chrono::system_clock::now()) == grpc::CompletionQueue::TIMEOUT)
			continue;
		binder.Update(tag, ok);
	}
	while (!service.quit());
	while (cq->Next(&tag, &ok));
	system("pause");
}