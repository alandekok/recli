# Data Types

Recli supports a few standard datatypes.

    BOOLEAN      0, 1, yes, no, true, false
    INTEGER      signed integer
    IPADDR       IP address
    MACADDR      Ethernet (MAC) address (00:01:02:03:04:05)
    STRING       Quoted string ("foo", or 'foo', or foo)
      DQSTRING   Double-quoted string "foo"
      SQSTRING   Single-quoted string 'foo'
      BQSTRING   Back-quoted string `foo`

These data types can be extended by editing "datatypes.c".
