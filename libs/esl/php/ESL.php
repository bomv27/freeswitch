<?php

/* ----------------------------------------------------------------------------
 * This file was automatically generated by SWIG (http://www.swig.org).
 * Version 1.3.35
 * 
 * This file is not intended to be easily readable and contains a number of 
 * coding conventions designed to improve portability and efficiency. Do not make
 * changes to this file unless you know what you are doing--modify the SWIG 
 * interface file instead. 
 * ----------------------------------------------------------------------------- */

// Try to load our extension if it's not already loaded.
if (!extension_loaded("ESL")) {
  if (strtolower(substr(PHP_OS, 0, 3)) === 'win') {
    if (!dl('php_ESL.dll')) return;
  } else {
    // PHP_SHLIB_SUFFIX is available as of PHP 4.3.0, for older PHP assume 'so'.
    // It gives 'dylib' on MacOS X which is for libraries, modules are 'so'.
    if (PHP_SHLIB_SUFFIX === 'PHP_SHLIB_SUFFIX' || PHP_SHLIB_SUFFIX === 'dylib') {
      if (!dl('ESL.so')) return;
    } else {
      if (!dl('ESL.'.PHP_SHLIB_SUFFIX)) return;
    }
  }
}



abstract class ESL {
	static function eslSetLogLevel($level) {
		eslSetLogLevel($level);
	}
}

/* PHP Proxy Classes */
class ESLevent {
	public $_cPtr=null;

	function __set($var,$value) {
		$func = 'ESLevent_'.$var.'_set';
		if (function_exists($func)) call_user_func($func,$this->_cPtr,$value);
	}

	function __isset($var) {
		return function_exists('ESLevent_'.$var.'_set');
	}

	function __get($var) {
		$func = 'ESLevent_'.$var.'_get';
		if (function_exists($func)) return call_user_func($func,$this->_cPtr);
		return null;
	}

	function __construct($type,$subclass_name=null) {
		switch (func_num_args()) {
		case 1: $r=new_ESLevent($type); break;
		default: $r=new_ESLevent($type,$subclass_name);
		}
		$this->_cPtr=$r;
	}

	function serialize($format=null) {
		switch (func_num_args()) {
		case 0: $r=ESLevent_serialize($this->_cPtr); break;
		default: $r=ESLevent_serialize($this->_cPtr,$format);
		}
		return $r;
	}

	function setPriority($priority=null) {
		switch (func_num_args()) {
		case 0: $r=ESLevent_setPriority($this->_cPtr); break;
		default: $r=ESLevent_setPriority($this->_cPtr,$priority);
		}
		return $r;
	}

	function getHeader($header_name) {
		return ESLevent_getHeader($this->_cPtr,$header_name);
	}

	function getBody() {
		return ESLevent_getBody($this->_cPtr);
	}

	function getType() {
		return ESLevent_getType($this->_cPtr);
	}

	function addBody($value) {
		return ESLevent_addBody($this->_cPtr,$value);
	}

	function addHeader($header_name,$value) {
		return ESLevent_addHeader($this->_cPtr,$header_name,$value);
	}

	function delHeader($header_name) {
		return ESLevent_delHeader($this->_cPtr,$header_name);
	}
}

class ESLconnection {
	public $_cPtr=null;

	function __construct($host_or_socket,$port=null,$password=null) {
		switch (func_num_args()) {
		case 1: $r=new_ESLconnection($host_or_socket); break;
		case 2: $r=new_ESLconnection($host_or_socket,$port); break;
		default: $r=new_ESLconnection($host_or_socket,$port,$password);
		}
		$this->_cPtr=$r;
	}

	function connected() {
		return ESLconnection_connected($this->_cPtr);
	}

	function getInfo() {
		$r=ESLconnection_getInfo($this->_cPtr);
		return is_resource($r) ? new ESLevent($r) : $r;
	}

	function send($cmd) {
		return ESLconnection_send($this->_cPtr,$cmd);
	}

	function sendRecv($cmd) {
		$r=ESLconnection_sendRecv($this->_cPtr,$cmd);
		return is_resource($r) ? new ESLevent($r) : $r;
	}

	function sendEvent($send_me) {
		return ESLconnection_sendEvent($this->_cPtr,$send_me);
	}

	function recvEvent() {
		$r=ESLconnection_recvEvent($this->_cPtr);
		return is_resource($r) ? new ESLevent($r) : $r;
	}

	function recvEventTimed($ms) {
		$r=ESLconnection_recvEventTimed($this->_cPtr,$ms);
		return is_resource($r) ? new ESLevent($r) : $r;
	}

	function filter($header,$value) {
		return ESLconnection_filter($this->_cPtr,$header,$value);
	}

	function events($etype,$value) {
		return ESLconnection_events($this->_cPtr,$etype,$value);
	}

	function execute($app,$arg=null,$uuid=null) {
		switch (func_num_args()) {
		case 1: $r=ESLconnection_execute($this->_cPtr,$app); break;
		case 2: $r=ESLconnection_execute($this->_cPtr,$app,$arg); break;
		default: $r=ESLconnection_execute($this->_cPtr,$app,$arg,$uuid);
		}
		return $r;
	}

	function setBlockingExecute($val) {
		return ESLconnection_setBlockingExecute($this->_cPtr,$val);
	}

	function setEventLock($val) {
		return ESLconnection_setEventLock($this->_cPtr,$val);
	}
}


?>
