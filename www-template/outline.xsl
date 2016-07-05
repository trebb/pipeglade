<?xml version="1.0" encoding="UTF-8" ?>
<xsl:stylesheet version="1.0"
                xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <!-- Extract the hierarchy of section headings from html -->
  <xsl:output method="text"/>

  <xsl:template match="script|noscript"/>

  <xsl:template match="@*|node()">
    <xsl:apply-templates/>
  </xsl:template>

  <xsl:template match="div[@class='section']/h1">
    <xsl:value-of select="normalize-space(.)"/>
    <xsl:text>&#xA;</xsl:text>
  </xsl:template>

  <xsl:template match="div[@class='subsection']/h2">
    <xsl:text>  </xsl:text>
    <xsl:value-of select="normalize-space(.)"/>
    <xsl:text>&#xA;</xsl:text>
  </xsl:template>

  <xsl:template match="div[@class='subsection']/dl/dt/text()">
    <xsl:text>    </xsl:text>
    <xsl:value-of select="normalize-space(.)"/>
    <xsl:text>&#xA;</xsl:text>
  </xsl:template>

  <xsl:template
      match="div[@class='subsection']/dl/dd/ul/li/b[@class='flag']">
    <xsl:variable name="action" select="normalize-space(.)"/>
    <xsl:if test="starts-with($action, ':')">
      <xsl:if test="string-length($action)>2">
        <xsl:text>      </xsl:text>
        <xsl:value-of select="$action"/>
        <xsl:text>&#xA;</xsl:text>
      </xsl:if>
    </xsl:if>
  </xsl:template>

</xsl:stylesheet>
