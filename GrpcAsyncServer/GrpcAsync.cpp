#include "GrpcAsync.h"
#include <boost/coroutine/symmetric_coroutine.hpp>

namespace GrpcAsync
{
	struct BoostCoroutine final : ICoroutine
	{
		using yield_t = boost::coroutines::symmetric_coroutine<void>::yield_type;
		using call_t = boost::coroutines::symmetric_coroutine<void>::call_type;
		BoostCoroutine(std::function<void()> fn) : yielder(nullptr),
		call([this, fn = move(fn)](yield_t& yield)
		{
			yielder = &yield;
			yield();
			fn();
		})
		{
			call();
		}

		void yield() override
		{
			(*yielder)();
		}
		
		void invoke() override
		{
			call();
		}
		
	private:
		yield_t* yielder;
		call_t call;
	};

	RPCBinding::RPCBinding(Yielder yielder) : Yielder(yielder), released(false), coroutine(new BoostCoroutine(std::bind(&RPCBinding::Execute, this)))
	{
		binder->bindings.emplace_back(this);
	}

	RPCBinding::RPCBinding(const RPCBinding& binding) : RPCBinding(binding.binder)
	{
		
	}

	bool RPCBinding::Update()
	{
		if (tag() != this)
			return false;
		binder->coroutine = coroutine.get();
		coroutine->invoke();
		return true;
	}

	void ServiceBinder::Update(void* tag, bool ok)
	{
		tag_ = tag;
		ok_ = ok;
		for (auto& binding : bindings)
			if (!binding->released && binding->Update())
				break;
		tag_ = nullptr;
		ok_ = false;
		bindings.remove_if([](auto& ptr) { return ptr->released; });
	}
	
}