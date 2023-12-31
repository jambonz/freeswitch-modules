COPYING
=======

This software is dual-licensed, allowing you to choose between the MIT License and the Affero General Public License (AGPL) Version 3.0 
under different conditions.

1. MIT License: 
   - This license applies only when the software is used exclusively in conjunction with a [jambonz](https://jambonz.org) voice server. 
   Specifically, that means all of the following conditions must be true:
   
      i) The software is compiled and dynamically loaded into a running Freeswitch process 
      that is dedicated to providing services to a jambonz server or cluster.

      ii) All incoming calls to the Freeswitch instance running this software are 
      controlled via an outbound ESL socket connection from Freeswitch 
      to a Node.js application running the [jambonz feature server](https://github.com/jambonz/jambonz-feature-server) application. 

   - For the full terms of the MIT License, see the [`LICENSE_MIT`](LICENSE_MIT) file.

2. AGPL Version 3.0: 
   - For all other uses not covered under the MIT License, the software is licensed under the AGPL Version 3.0.
   - For the full terms of the AGPL Version 3.0, see the [`LICENSE_AGPL-3.0`](./LICENSE_AGPL-3.0) file.

Please refer to the individual license files for the complete terms and conditions 
of each license. Your use of this software constitutes agreement to the terms of these licenses. 
If you have any questions regarding the licensing of this software, please consult a legal expert.

