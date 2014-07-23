#include <vector>
#include <boost/bind.hpp>
#include <boost/lexical_cast.hpp>

#include "TcpConnection.h"
#include "../../Database/Database.h"
#include "../Output/OutputBuffer.h"

// DEBUG ONLY FIXME
#include <sstream>
#include <iostream>
#include <string>

using std::vector;
using std::cout;
using std::endl;

namespace TcpServer_Namespace {

TcpConnection::TcpConnection(boost::asio::io_service& ioService,
    Database& database)
  : strand_(ioService),
    socket_(ioService),
    database_(database),
    queryDelim_(';')
{
  printf("\nCreating connection\n");
}

TcpConnection::~TcpConnection() {
    std::cout << "End of conn" << std::endl;
    boost::system::error_code ignored_ec;
    socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored_ec);
    socket_.close();
}

boost::asio::ip::tcp::socket& TcpConnection::socket()
{
  return socket_;
}

void TcpConnection::start()
{
  /*
  socket_.async_read_some(boost::asio::buffer(buffer_),
      strand_.wrap(
        boost::bind(&TcpConnection::handle_read, shared_from_this(),
          boost::asio::placeholders::error,
          boost::asio::placeholders::bytes_transferred)));
  */
  boost::asio::async_read_until(socket_, buffer_,
      queryDelim_,
      strand_.wrap(
        boost::bind(&TcpConnection::handleRead, shared_from_this(),
          boost::asio::placeholders::error,
          boost::asio::placeholders::bytes_transferred))); //);
}

void TcpConnection::handleRead(const boost::system::error_code& e,
    std::size_t bytes_transferred)
{
    if (!e) {
        const char* string = boost::asio::buffer_cast<const char*>(buffer_.data());
        std::string request(string, bytes_transferred);
        buffer_.consume(buffer_.size());
        OutputBuffer outputBuffer;
        cout << "Before process" << endl;
        bool isValid = database_.processRequest(request, outputBuffer);
        cout << "Req is valid: " << isValid << endl;
        writeOutput(outputBuffer);
    }

    //if (isValid) { // Request is good
    //    char data [24];
    //    int bytes = 16;
    //    long long int numRows = -1;
    //    memcpy(data, &bytes,sizeof(int));
    //    memcpy(data + 4, &numRows,sizeof(long long int));
    //    bytes = 4;
    //    memcpy(data + 12, &bytes,sizeof(int));
    //    data[16] = 'g';
    //    data[17] = 'o';
    //    data[18] = 'o';
    //    data[19] = 'd';
    //    bytes = 0;
    //    memcpy(data + 20, &bytes,sizeof(int));
    //    
    //    /*
    //  std::ostringstream ss;
    //  ss << boost::lexical_cast<std::string>(buffer_);
    //  std::string input = ss.str().substr(0,bytes_transferred);
    //  std::cout << "TcpConnection::handle_read() : " << input << std::endl;
    //  request_handler_.handle_request(request_, reply_);
    //  */
    //  boost::asio::async_write(socket_, boost::asio::buffer(data) /*reply_.to_buffers()*/,
    //      strand_.wrap(
    //        boost::bind(&TcpConnection::handle_write, shared_from_this(),
    //          boost::asio::placeholders::error)));
    // Bad request
    else if (0) //!result)
    {
      /*
      reply_ = reply::stock_reply(reply::bad_request);
      boost::asio::async_write(socket_, reply_.to_buffers(),
          strand_.wrap(
            boost::bind(&TcpConnection::handle_write, shared_from_this(),
              boost::asio::placeholders::error)));
      */
    }
    else
    {
      /*
      socket_.async_read_some(boost::asio::buffer(buffer_),
          strand_.wrap(
            boost::bind(&TcpConnection::handle_read, shared_from_this(),
              boost::asio::placeholders::error,
              boost::asio::placeholders::bytes_transferred)));
      */
    }

  // If an error occurs then no new asynchronous operations are started. This
  // means that all shared_ptr references to the TcpConnection object will
  // disappear and the object will be destroyed automatically after this
  // handler returns. The TcpConnection class's destructor closes the socket.
}

void TcpConnection::writeOutput(OutputBuffer &outputBuffer) {
    cout << "Writing output " << endl;
    if (outputBuffer.size() > 0) {
        const vector <char> &subBuffer = outputBuffer.front();
        int subBufferSize = subBuffer.size();
        cout << "SubBufferSize: " << subBufferSize << endl;
        /*
        char * dataCharPtr = reinterpret_cast <char *> (subBufferSize);
        subBuffer.insert(subBuffer.begin(), dataCharPtr, dataCharPtr + 4);
        */

      boost::asio::buffer boostBuffer (&subBuffer[0],subBufferSize);

      outputBuffer.pop();

      boost::asio::async_write(socket_, boostBuffer,
          strand_.wrap(
            boost::bind(&TcpConnection::handleWrite, shared_from_this(),
              boost::asio::placeholders::error, boost::ref(outputBuffer))));

    }
    else {
        start();
    }
}



void TcpConnection::handleWrite(const boost::system::error_code& e, OutputBuffer &outputBuffer)
{
  if (!e)
  {
      writeOutput(outputBuffer);
    /*
      boost::asio::async_read_until(socket_, buffer_,
          queryDelim_,
          strand_.wrap(
            boost::bind(&TcpConnection::handleRead, shared_from_this(),
              boost::asio::placeholders::error,
              boost::asio::placeholders::bytes_transferred))); //);
        */
    // Initiate graceful TcpConnection closure.
      //std::cout << "Closing connection" << std::endl;
    //boost::system::error_code ignored_ec;
    //socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored_ec);
  }
  else {
      std::cout << "socket error: " << e << std::endl;
      //delete this;
      
  }

  // No new asynchronous operations are started. This means that all shared_ptr
  // references to the TcpConnection object will disappear and the object will be
  // destroyed automatically after this handler returns. The TcpConnection class's
  // destructor closes the socket.
}

} // namespace TcpServer_Namespace
