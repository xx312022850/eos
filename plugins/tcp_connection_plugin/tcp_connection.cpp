#include <eos/tcp_connection_plugin/tcp_connection.hpp>

#include <fc/log/logger.hpp>

#include <boost/asio.hpp>

namespace eos {

tcp_connection::tcp_connection(boost::asio::ip::tcp::socket s) :
   socket(std::move(s)),
   strand(socket.get_io_service())
{
   ilog("tcp_connection created");

   fc::ecc::private_key priv_key = fc::ecc::private_key::generate();
   
   fc::ecc::public_key pub = priv_key.get_public_key();
   fc::ecc::public_key_data pkd = pub.serialize();

   size_t wrote = boost::asio::write(socket, boost::asio::buffer(pkd.begin(), pkd.size()));
   if(wrote != pkd.size()) {
      handle_failure();
      return;
   }
   boost::asio::async_read(socket, boost::asio::buffer(rxbuffer, pkd.size()), [this, priv_key](auto ec, auto r) {
      finish_key_exchange(ec, r, priv_key);
   });
   //read();
}

tcp_connection::~tcp_connection() {
   ilog("Connection destroyed"); //XXX debug
}

void tcp_connection::handle_failure() {
    socket.cancel();
    socket.close();
    if(!disconnected_fired)
       on_disconnected_sig();
    disconnected_fired = true;
}

void tcp_connection::finish_key_exchange(boost::system::error_code ec, size_t red, fc::ecc::private_key priv_key) {
   //check ec/wrote
   ilog("finished key exchange");
   fc::ecc::public_key_data* rpub = (fc::ecc::public_key_data*)rxbuffer;
   if(ec || red != rpub->size()) {
       handle_failure();
       return;
   }
   fc::sha512 shared_secret = priv_key.get_shared_secret(*rpub);

   auto ss_data = shared_secret.data();
   auto ss_data_size = shared_secret.data_size();

   sending_aes_enc_ctx.init  (fc::sha256::hash(ss_data, ss_data_size), fc::city_hash_crc_128(ss_data, ss_data_size));
   receiving_aes_dec_ctx.init(fc::sha256::hash(ss_data, ss_data_size), fc::city_hash_crc_128(ss_data, ss_data_size));

}

void tcp_connection::read() {
   socket.async_read_some(boost::asio::buffer(rxbuffer), strand.wrap([this](auto ec, auto r) {
      read_ready(ec, r);
   }));
}

void tcp_connection::read_ready(boost::system::error_code ec, size_t red) {
   ilog("read ${r} bytes!", ("r",red));
   if(ec) {
      handle_failure();
      return;
   }

  // queuedOutgoing.emplace_front(rxbuffer, rxbuffer+red);

   ///XXX Note this is wrong Wrong WRONG as the async sends don't guarantee ordering;
   //     there needs to be a work queue of sorts.

   /*boost::asio::async_write(socket,
                            boost::asio::buffer(queuedOutgoing.front().data(), queuedOutgoing.front().size()),
                            strand.wrap([this, it=queuedOutgoing.begin()](auto ec, auto s) {
                               send_complete(ec, s, it);
                            }));*/

   read();
}

void tcp_connection::send_complete(boost::system::error_code ec, size_t sent, std::list<std::vector<uint8_t>>::iterator it) {
   if(ec) {
      handle_failure();
      return;
   }
   //sent should always == sent requested unless error, right? because _write() not _send_some()etc
   assert(sent == it->size());
   queuedOutgoing.erase(it);
}

bool tcp_connection::disconnected() {
   return !socket.is_open();
}

connection tcp_connection::on_disconnected(const signal<void()>::slot_type& slot) {
   return on_disconnected_sig.connect(slot);
}

}